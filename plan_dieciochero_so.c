#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <poll.h>

#define MAX_ID_LEN 32
#define MAX_NOMBRE_LEN 64
#define MAX_MSG_LEN 256
#define MAX_LINEA 1024
#define DUR_MIN_MS 100
#define DUR_MAX_MS 5000
#define RUTA_LOG "planificador.log"

typedef enum {
    PENDIENTE,
    LISTA,
    EJECUTANDO,
    COMPLETADA,
    FALLIDA,
    ABORTADA
} Estado;

typedef struct {
    char id[MAX_ID_LEN];
    char nombre[MAX_NOMBRE_LEN];
    int tiempo_ms;

    int *deps; int n_deps; int cap_deps;
    int *hijos; int n_hijos; int cap_hijos;

    int deps_pendientes;
    Estado estado;

    pid_t pid;
    int fd_salida;
    char msg_entrada[MAX_MSG_LEN];
} Nodo;

typedef struct HashEntry {
    char id[MAX_ID_LEN];
    int idx;
    struct HashEntry *sig;
} HashEntry;

static HashEntry **g_tabla_hash = NULL;
static size_t g_tabla_cap = 0;

static Nodo *g_nodos = NULL;
static int g_n_nodos = 0;

static int *g_cola = NULL;
static int g_cola_head = 0, g_cola_tail = 0, g_cola_count = 0, g_cola_cap = 0;
static volatile sig_atomic_t g_interrumpido = 0;
static FILE *g_log = NULL;

static char *recortar(char *s) {
    while (isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    char *fin = s + strlen(s) - 1;
    while (fin > s && isspace((unsigned char)*fin)) { *fin = '\0'; fin--; }
    return s;
}

static int partir_linea(char *linea, char **c1, char **c2, char **c3, char **c4) {
    char *p1 = strchr(linea, ':');
    if (!p1) return -1;
    *p1 = '\0';
    char *p2 = strchr(p1 + 1, ':');
    if (!p2) return -1;
    *p2 = '\0';
    char *p3 = strchr(p2 + 1, ':');
    if (!p3) return -1;
    *p3 = '\0';

    *c1 = linea;
    *c2 = p1 + 1;
    *c3 = p2 + 1;
    *c4 = p3 + 1;
    return 0;
}

static void concatenar_seguro(char *dest, size_t cap, const char *src) {
    size_t len = strlen(dest);
    if (len + 1 >= cap) return;
    size_t restante = cap - len - 1;

    if (len > 0 && restante > 1) {
        dest[len] = ';';
        dest[len + 1] = '\0';
        len++;
        restante--;
    }
    snprintf(dest + len, restante + 1, "%s", src);
}

static int abrir_log(const char *ruta) {
    g_log = fopen(ruta, "w");
    if (!g_log) {
        fprintf(stderr,
                "Aviso: no se pudo crear el archivo de log '%s': %s "
                "(se continua solo con la salida por pantalla)\n",
                ruta, strerror(errno));
        return -1;
    }
    return 0;
}

static void cerrar_log(void) {
    if (g_log) {
        fclose(g_log);
        g_log = NULL;
    }
}

static void marca_tiempo(char *buf, size_t cap) {
    time_t ahora = time(NULL);
    struct tm tm_local;
    localtime_r(&ahora, &tm_local);
    strftime(buf, cap, "%H:%M:%S", &tm_local);
}

static void registrar(const char *fmt, ...) {
    va_list args_pantalla, args_log;
    va_start(args_pantalla, fmt);
    va_copy(args_log, args_pantalla);

    vprintf(fmt, args_pantalla);
    va_end(args_pantalla);

    if (g_log) {
        char marca[16];
        marca_tiempo(marca, sizeof marca);
        fprintf(g_log, "[%s] ", marca);
        vfprintf(g_log, fmt, args_log);
        fflush(g_log);
    }
    va_end(args_log);
}

static void registrar_error(const char *fmt, ...) {
    va_list args_pantalla, args_log;
    va_start(args_pantalla, fmt);
    va_copy(args_log, args_pantalla);

    vfprintf(stderr, fmt, args_pantalla);
    va_end(args_pantalla);

    if (g_log) {
        char marca[16];
        marca_tiempo(marca, sizeof marca);
        fprintf(g_log, "[%s] [ERROR] ", marca);
        vfprintf(g_log, fmt, args_log);
        fflush(g_log);
    }
    va_end(args_log);
}

static unsigned long hash_cadena(const char *s) {
    unsigned long h = 5381;
    int c;
    while ((c = (unsigned char)*s++)) h = ((h << 5) + h) + (unsigned long)c;
    return h;
}

static int hash_iniciar(size_t n_estimado) {
    g_tabla_cap = n_estimado * 2 + 8;
    g_tabla_hash = calloc(g_tabla_cap, sizeof(HashEntry *));
    return g_tabla_hash ? 0 : -1;
}

static int hash_buscar(const char *id) {
    unsigned long h = hash_cadena(id) % g_tabla_cap;
    for (HashEntry *e = g_tabla_hash[h]; e; e = e->sig) {
        if (strcmp(e->id, id) == 0) return e->idx;
    }
    return -1;
}

static int hash_insertar(const char *id, int idx) {
    unsigned long h = hash_cadena(id) % g_tabla_cap;
    HashEntry *e = malloc(sizeof(HashEntry));
    if (!e) return -1;
    snprintf(e->id, sizeof e->id, "%s", id);
    e->idx = idx;
    e->sig = g_tabla_hash[h];
    g_tabla_hash[h] = e;
    return 0;
}

static void hash_liberar(void) {
    if (!g_tabla_hash) return;
    for (size_t i = 0; i < g_tabla_cap; i++) {
        HashEntry *e = g_tabla_hash[i];
        while (e) {
            HashEntry *sig = e->sig;
            free(e);
            e = sig;
        }
    }
    free(g_tabla_hash);
    g_tabla_hash = NULL;
}

static void agregar_dep(Nodo *n, int idx) {
    if (n->n_deps == n->cap_deps) {
        n->cap_deps = n->cap_deps ? n->cap_deps * 2 : 4;
        int *tmp = realloc(n->deps, (size_t)n->cap_deps * sizeof(int));
        if (!tmp) return;
        n->deps = tmp;
    }
    n->deps[n->n_deps++] = idx;
}

static void agregar_hijo(Nodo *n, int idx) {
    if (n->n_hijos == n->cap_hijos) {
        n->cap_hijos = n->cap_hijos ? n->cap_hijos * 2 : 4;
        int *tmp = realloc(n->hijos, (size_t)n->cap_hijos * sizeof(int));
        if (!tmp) return;
        n->hijos = tmp;
    }
    n->hijos[n->n_hijos++] = idx;
}

static void liberar_nodos(Nodo *nodos, int n) {
    for (int i = 0; i < n; i++) {
        free(nodos[i].deps);
        free(nodos[i].hijos);
    }
    free(nodos);
}

static int cargar_plan(const char *ruta, Nodo **out_nodos, int *out_n) {
    FILE *f = fopen(ruta, "r");
    if (!f) {
        registrar_error("No se pudo abrir '%s': %s\n", ruta, strerror(errno));
        return -1;
    }

    int cap = 16;
    Nodo *nodos = calloc((size_t)cap, sizeof(Nodo));
    char **deps_crudas = calloc((size_t)cap, sizeof(char *));
    int n = 0;

    char linea[MAX_LINEA];
    int num_linea = 0;

    while (fgets(linea, sizeof linea, f)) {
        num_linea++;
        linea[strcspn(linea, "\r\n")] = '\0';
        char *t = recortar(linea);
        if (*t == '\0' || *t == '#') continue;

        char *c1, *c2, *c3, *c4;
        char copia_linea[MAX_LINEA];
        snprintf(copia_linea, sizeof copia_linea, "%s", t);

        if (partir_linea(copia_linea, &c1, &c2, &c3, &c4) != 0) {
            registrar_error("Linea %d: formato invalido, se ignora -> %s\n", num_linea, t);
            continue;
        }

        char *id        = recortar(c1);
        char *nombre    = recortar(c2);
        char *tiempo_tx = recortar(c3);
        char *deps_tx   = recortar(c4);

        if (*id == '\0') {
            registrar_error("Linea %d: falta ID_Actividad, se ignora\n", num_linea);
            continue;
        }

        if (n == cap) {
            int nueva_cap = cap * 2;
            Nodo *tmp = realloc(nodos, (size_t)nueva_cap * sizeof(Nodo));
            char **tmp2 = realloc(deps_crudas, (size_t)nueva_cap * sizeof(char *));
            if (!tmp || !tmp2) {
                registrar_error("Sin memoria al cargar el plan\n");
                fclose(f);
                free(tmp ? tmp : nodos);
                free(tmp2 ? tmp2 : deps_crudas);
                return -1;
            }
            nodos = tmp;
            deps_crudas = tmp2;
            memset(nodos + cap, 0, (size_t)(nueva_cap - cap) * sizeof(Nodo));
            memset(deps_crudas + cap, 0, (size_t)(nueva_cap - cap) * sizeof(char *));
            cap = nueva_cap;
        }

        Nodo *nd = &nodos[n];
        snprintf(nd->id, sizeof nd->id, "%s", id);
        snprintf(nd->nombre, sizeof nd->nombre, "%s", nombre);

        if (*tiempo_tx == '\0') {
            nd->tiempo_ms = DUR_MIN_MS + rand() % (DUR_MAX_MS - DUR_MIN_MS + 1);
        } else {
            nd->tiempo_ms = atoi(tiempo_tx);
            if (nd->tiempo_ms <= 0) {
                nd->tiempo_ms = DUR_MIN_MS + rand() % (DUR_MAX_MS - DUR_MIN_MS + 1);
            }
        }

        nd->estado = PENDIENTE;
        nd->pid = -1;
        nd->fd_salida = -1;
        nd->msg_entrada[0] = '\0';

        deps_crudas[n] = strdup(deps_tx);
        n++;
    }
    fclose(f);

    if (n == 0) {
        registrar_error("El plan no contiene actividades validas.\n");
        free(nodos);
        free(deps_crudas);
        return -1;
    }

    if (hash_iniciar((size_t)n) != 0) {
        registrar_error("No se pudo inicializar la tabla de indices\n");
        for (int i = 0; i < n; i++) free(deps_crudas[i]);
        free(deps_crudas);
        free(nodos);
        return -1;
    }

    int error_ids = 0;
    for (int i = 0; i < n; i++) {
        if (hash_buscar(nodos[i].id) != -1) {
            registrar_error("ID de actividad duplicado: %s\n", nodos[i].id);
            error_ids = 1;
            continue;
        }
        hash_insertar(nodos[i].id, i);
    }

    int error_deps = 0;
    if (!error_ids) {
        for (int i = 0; i < n; i++) {
            char *t = recortar(deps_crudas[i]);
            size_t len = strlen(t);
            if (len >= 2 && t[0] == '[' && t[len - 1] == ']') {
                t[len - 1] = '\0';
                t++;
            }
            if (*t != '\0') {
                char *guardar = NULL;
                char *copia = strdup(t);
                char *tok = strtok_r(copia, ",", &guardar);
                while (tok) {
                    char *dep_id = recortar(tok);
                    if (*dep_id != '\0') {
                        int j = hash_buscar(dep_id);
                        if (j < 0) {
                            registrar_error("La actividad %s depende de un ID inexistente: %s\n",
                                    nodos[i].id, dep_id);
                            error_deps = 1;
                        } else if (j == i) {
                            registrar_error("La actividad %s no puede depender de si misma\n",
                                    nodos[i].id);
                            error_deps = 1;
                        } else {
                            agregar_dep(&nodos[i], j);
                            agregar_hijo(&nodos[j], i);
                        }
                    }
                    tok = strtok_r(NULL, ",", &guardar);
                }
                free(copia);
            }
        }
    }

    for (int i = 0; i < n; i++) free(deps_crudas[i]);
    free(deps_crudas);
    hash_liberar();

    if (error_ids || error_deps) {
        liberar_nodos(nodos, n);
        return -1;
    }

    for (int i = 0; i < n; i++) nodos[i].deps_pendientes = nodos[i].n_deps;

    *out_nodos = nodos;
    *out_n = n;
    return 0;
}

static int cola_iniciar(int capacidad) {
    g_cola_cap = capacidad + 1;
    g_cola = malloc((size_t)g_cola_cap * sizeof(int));
    g_cola_head = g_cola_tail = g_cola_count = 0;
    return g_cola ? 0 : -1;
}

static void cola_push(int idx) {
    g_cola[g_cola_tail] = idx;
    g_cola_tail = (g_cola_tail + 1) % g_cola_cap;
    g_cola_count++;
}

static int cola_pop(void) {
    int idx = g_cola[g_cola_head];
    g_cola_head = (g_cola_head + 1) % g_cola_cap;
    g_cola_count--;
    return idx;
}

static void manejador_sigint(int signo) {
    (void)signo;
    g_interrumpido = 1;
}

static void instalar_manejador_sigint(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = manejador_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
}

static void abortar_todo(void) {
    registrar_error("\n[SEREMI] Señal SIGINT recibida: abortando todas las actividades...\n");
    for (int i = 0; i < g_n_nodos; i++) {
        if (g_nodos[i].estado == EJECUTANDO && g_nodos[i].pid > 0) {
            kill(g_nodos[i].pid, SIGTERM);
        }
    }
    for (int i = 0; i < g_n_nodos; i++) {
        if (g_nodos[i].estado == EJECUTANDO) {
            int status;
            waitpid(g_nodos[i].pid, &status, 0);
            if (g_nodos[i].fd_salida >= 0) close(g_nodos[i].fd_salida);
            g_nodos[i].estado = ABORTADA;
        } else if (g_nodos[i].estado == PENDIENTE || g_nodos[i].estado == LISTA) {
            g_nodos[i].estado = ABORTADA;
        }
    }
}

static void dormir_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void ejecutar_actividad(const Nodo *nodo, int fd_entrada, int fd_salida) {
    char entrada[MAX_MSG_LEN];
    ssize_t leidos = read(fd_entrada, entrada, sizeof(entrada) - 1);
    if (leidos < 0) leidos = 0;
    entrada[leidos] = '\0';
    close(fd_entrada);

    dormir_ms(nodo->tiempo_ms);

    char salida[MAX_MSG_LEN];
    snprintf(salida, sizeof salida, "OK:%s:%s:listo en %dms (insumo: %.60s)",
             nodo->id, nodo->nombre, nodo->tiempo_ms,
             entrada[0] ? entrada : "sin insumo previo");

    ssize_t w = write(fd_salida, salida, strlen(salida));
    (void)w;

    close(fd_salida);
    _exit(EXIT_SUCCESS);
}

static int lanzar_actividad(Nodo *nodo) {
    int fd_entrada[2], fd_salida[2];

    if (pipe(fd_entrada) < 0) {
        registrar_error("pipe: %s\n", strerror(errno));
        return -1;
    }
    if (pipe(fd_salida) < 0) {
        registrar_error("pipe: %s\n", strerror(errno));
        close(fd_entrada[0]); close(fd_entrada[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        registrar_error("fork: %s\n", strerror(errno));
        close(fd_entrada[0]); close(fd_entrada[1]);
        close(fd_salida[0]); close(fd_salida[1]);
        return -1;
    }

    if (pid == 0) {
        close(fd_entrada[1]);
        close(fd_salida[0]);
        signal(SIGINT, SIG_DFL);
        ejecutar_actividad(nodo, fd_entrada[0], fd_salida[1]);
        _exit(EXIT_FAILURE);
    }

    close(fd_entrada[0]);
    close(fd_salida[1]);

    if (nodo->msg_entrada[0] != '\0') {
        ssize_t w = write(fd_entrada[1], nodo->msg_entrada, strlen(nodo->msg_entrada));
        (void)w;
    }
    close(fd_entrada[1]);

    nodo->pid = pid;
    nodo->fd_salida = fd_salida[0];
    nodo->estado = EJECUTANDO;
    return 0;
}

static void abortar_rama(int idx_fallido, int *completados_terminal) {
    int cap_pila = g_n_nodos;
    int *pila = malloc((size_t)cap_pila * sizeof(int));
    if (!pila) return;
    int tope = 0;

    for (int k = 0; k < g_nodos[idx_fallido].n_hijos; k++) {
        pila[tope++] = g_nodos[idx_fallido].hijos[k];
    }

    while (tope > 0) {
        int j = pila[--tope];
        if (g_nodos[j].estado != PENDIENTE) continue;
        g_nodos[j].estado = ABORTADA;
        (*completados_terminal)++;
        registrar("[ABORT] %-12s %-24s (rama dependiente de una actividad fallida)\n",
               g_nodos[j].id, g_nodos[j].nombre);

        for (int k = 0; k < g_nodos[j].n_hijos; k++) {
            if (tope >= cap_pila) {
                cap_pila *= 2;
                int *tmp = realloc(pila, (size_t)cap_pila * sizeof(int));
                if (!tmp) { free(pila); return; }
                pila = tmp;
            }
            pila[tope++] = g_nodos[j].hijos[k];
        }
    }
    free(pila);
}

static void procesar_finalizacion(int idx, int *running_count, int *completados_terminal) {
    Nodo *nodo = &g_nodos[idx];
    char buf[MAX_MSG_LEN];

    ssize_t leidos = read(nodo->fd_salida, buf, sizeof(buf) - 1);
    if (leidos > 0) {
        buf[leidos] = '\0';
    } else {
        snprintf(buf, sizeof buf, "FAIL:%s:%s:proceso finalizo sin reportar resultado",
                 nodo->id, nodo->nombre);
    }
    close(nodo->fd_salida);
    nodo->fd_salida = -1;

    int status = 0;
    waitpid(nodo->pid, &status, 0);

    int ok = (strncmp(buf, "OK:", 3) == 0) && WIFEXITED(status) && WEXITSTATUS(status) == 0;

    (*running_count)--;
    (*completados_terminal)++;

    if (ok) {
        nodo->estado = COMPLETADA;
        registrar("[OK]    %-12s %-24s %s\n", nodo->id, nodo->nombre, buf);

        for (int k = 0; k < nodo->n_hijos; k++) {
            Nodo *hijo = &g_nodos[nodo->hijos[k]];
            if (hijo->estado != PENDIENTE) continue;
            concatenar_seguro(hijo->msg_entrada, sizeof hijo->msg_entrada, buf);
            hijo->deps_pendientes--;
            if (hijo->deps_pendientes == 0) {
                hijo->estado = LISTA;
                cola_push(nodo->hijos[k]);
            }
        }
    } else {
        nodo->estado = FALLIDA;
        registrar("[FALLO] %-12s %-24s %s\n", nodo->id, nodo->nombre, buf);
        abortar_rama(idx, completados_terminal);
    }
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Uso: %s plan.txt K\n", argv[0]);
        return EXIT_FAILURE;
    }

    abrir_log(RUTA_LOG);

    char *fin_endptr = NULL;
    long k_long = strtol(argv[2], &fin_endptr, 10);
    if (*fin_endptr != '\0' || k_long <= 0) {
        registrar_error("K debe ser un entero positivo\n");
        cerrar_log();
        return EXIT_FAILURE;
    }
    int K = (int)k_long;

    srand((unsigned)time(NULL));

    Nodo *nodos = NULL;
    int n_nodos = 0;
    if (cargar_plan(argv[1], &nodos, &n_nodos) != 0) {
        cerrar_log();
        return EXIT_FAILURE;
    }
    g_nodos = nodos;
    g_n_nodos = n_nodos;

    instalar_manejador_sigint();

    if (cola_iniciar(n_nodos) != 0) {
        registrar_error("Sin memoria para la cola de listos\n");
        liberar_nodos(nodos, n_nodos);
        cerrar_log();
        return EXIT_FAILURE;
    }

    for (int i = 0; i < n_nodos; i++) {
        if (nodos[i].n_deps == 0) {
            nodos[i].estado = LISTA;
            cola_push(i);
        }
    }

    int running_count = 0;
    int completados_terminal = 0;
    int hubo_fallos = 0;

    struct pollfd *fds_poll = malloc((size_t)K * sizeof(struct pollfd));
    int *indices_poll = malloc((size_t)K * sizeof(int));

    if (!fds_poll || !indices_poll) {
        registrar_error("Error reservando memoria para monitoreo I/O\n");
        free(fds_poll); free(indices_poll);
        free(g_cola);
        liberar_nodos(nodos, n_nodos);
        cerrar_log();
        return EXIT_FAILURE;
    }

    registrar("=== Planificador Dieciochero === (%d actividades, K=%d)\n\n", n_nodos, K);

    while (completados_terminal < n_nodos) {
        if (g_interrumpido) {
            abortar_todo();
            hubo_fallos = 1;
            completados_terminal = n_nodos;
            break;
        }

        while (running_count < K && g_cola_count > 0) {
            int idx = cola_pop();
            if (lanzar_actividad(&nodos[idx]) == 0) {
                running_count++;
            } else {
                cola_push(idx);
                break;
            }
        }

        if (running_count == 0 && g_cola_count == 0 && completados_terminal < n_nodos) {
            registrar_error(
                "Error: quedan %d actividades que nunca podran ejecutarse "
                "(dependencias irresolubles o ciclo en el plan).\n",
                n_nodos - completados_terminal);
            for (int i = 0; i < n_nodos; i++) {
                if (nodos[i].estado == PENDIENTE) {
                    nodos[i].estado = ABORTADA;
                    completados_terminal++;
                }
            }
            hubo_fallos = 1;
            break;
        }

        int nfds = 0;
        for (int i = 0; i < n_nodos; i++) {
            if (nodos[i].estado == EJECUTANDO && nodos[i].fd_salida >= 0) {
                fds_poll[nfds].fd = nodos[i].fd_salida;
                fds_poll[nfds].events = POLLIN;
                fds_poll[nfds].revents = 0;
                indices_poll[nfds] = i;
                nfds++;
                if (nfds == K) break;
            }
        }

        if (nfds > 0) {
            int ret = poll(fds_poll, (nfds_t)nfds, 200);
            if (ret < 0) {
                if (errno == EINTR) continue;
                registrar_error("poll: %s\n", strerror(errno));
                break;
            }

            if (ret > 0) {
                for (int j = 0; j < nfds; j++) {
                    if (fds_poll[j].revents & (POLLIN | POLLHUP | POLLERR)) {
                        procesar_finalizacion(indices_poll[j], &running_count, &completados_terminal);
                    }
                }
            }
        }
    }

    int n_ok = 0, n_fail = 0, n_abort = 0;
    for (int i = 0; i < n_nodos; i++) {
        switch (nodos[i].estado) {
            case COMPLETADA: n_ok++; break;
            case FALLIDA:    n_fail++; break;
            case ABORTADA:   n_abort++; break;
            default: break;
        }
    }
    if (n_fail > 0 || n_abort > 0) hubo_fallos = 1;

    registrar("\n=== Resumen ===\n");
    registrar("Completadas: %d | Fallidas: %d | Abortadas: %d | Total: %d\n",
           n_ok, n_fail, n_abort, n_nodos);

    free(fds_poll);
    free(indices_poll);
    free(g_cola);
    liberar_nodos(nodos, n_nodos);
    cerrar_log();

    return hubo_fallos ? EXIT_FAILURE : EXIT_SUCCESS;
}