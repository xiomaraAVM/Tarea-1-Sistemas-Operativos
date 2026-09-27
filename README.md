# Planificador Dieciochero (Sistemas Operativos)

Este proyecto es un planificador de tareas y actividades en C con soporte para concurrencia de K procesos simultáneos, manejo de dependencias mediante gráficos dirigidos acyclicos (DAG)

## Requisitos

* Compilador `gcc` con soporte para el estándar C17.
* Herramienta `make`.
* Sistema operativo basado en POSIX (Linux / macOS / WSL).

* **`plan_dieciochero_so.c`**: Código fuente principal de la aplicación.
* **`Makefile`**: Script de automatización de compilación.
* **`plan.txt`**: Archivo de entrada con las actividades y sus dependencias (se consideraron 17 actividades como base, pero debe cumplir para las 10000).
* **`README.md`**: Documentación del proyecto.

## Ejecutar

make
./planificador plan.txt K
