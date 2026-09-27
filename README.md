# Planificador Dieciochero (Sistemas Operativos)

Este proyecto es un planificador de tareas y actividades en C con soporte para concurrencia de K procesos simultáneos, manejo de dependencias mediante gráficos dirigidos

## Requisitos

* Compilador `gcc` con soporte para el estándar C17.
* Herramienta `make`.
* Sistema operativo basado en POSIX (Se usó Ubuntu para esta tarea).

##Funciones
* **`cargar_plan(const char *ruta, Nodo **out_nodos, int *out_n)`**: Parsea el archivo de entrada (`plan.txt`), extrae las dependencias de cada actividad e inicializa las estructuras del grafo.
* **`hash_iniciar` / `hash_buscar` / `hash_insertar`**: Implementan una tabla Hash para mapear los IDs alfanuméricos de las actividades a sus índices numéricos en $O(1)$.
* **`lanzar_actividad(Nodo *nodo)`**: Crea un nuevo proceso hijo mediante `fork()` y establece las tuberías de comunicación (`pipe`) de entrada/salida para la actividad.
* **`procesar_finalizacion(...)`**: Lee la salida del proceso hijo, evalúa si la ejecución fue exitosa o fallida, actualiza el estado de las dependencias e ingresa las nuevas actividades listas a la cola.
* **`abortar_rama(int idx_fallido, int *completados_terminal)`**: Implementa una búsqueda por profundidad (DFS) para marcar como `ABORTADA` únicamente la rama de actividades que dependen (directa o indirectamente) de una actividad fallida, permitiendo que las ramas independientes continúen.
* **`instalar_manejador_sigint()` / `abortar_todo()`**: Captura la señal `SIGINT` (Ctrl+C) para terminar de forma ordenada todos los procesos en ejecución mediante `SIGTERM`.

##Formato

* **`plan_dieciochero_so.c`**: Código fuente principal de la aplicación.
* **`Makefile`**: Script de automatización de compilación.
* **`plan.txt`**: Archivo de entrada con las actividades y sus dependencias (se consideraron 17 actividades como base, pero debe cumplir para las 10000).
* **`README.md`**: Documentación del proyecto.

* Adicionalmente, se crea un archivo planificador.log para el registro formal de la ejecución de la aplicación (este puede ser eliminado en la limpieza mediante rm)
## Ejecutar

make

./planificador plan.txt K

## Limpieza

make clean

rm -f planificador.log
