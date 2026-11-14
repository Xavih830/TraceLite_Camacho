# Historial de Cambios (Updates) — TraceLite

Este archivo registra cronológicamente cada cambio implementado en el proyecto. Ninguna entrada previa debe ser sobrescrita o eliminada.

---

## [2026-11-09] Configuración inicial de `.gitignore` e historial `Updates.md`

### Qué se implementó
- Se actualizó el archivo `.gitignore` en la raíz del repositorio para excluir:
  - La carpeta `docs/` (documentación interna de planificación e historial).
  - Directorios de compilación del proyecto (`bin/`, `build/`).
  - Archivos sensibles y de credenciales (`.env`, `.env.*`, `*.pem`, `*.key`, `*.crt`, `secrets/`).
  - Archivos temporales de sistema operativo, editores y volcados de memoria (`.DS_Store`, `Thumbs.db`, `.vscode/`, `.idea/`, `*.swp`, `core`, `vgcore.*`).
- Se creó el archivo `docs/Updates.md` para llevar el registro histórico e inmutable de todos los cambios realizados durante el desarrollo.

---

## [2026-11-09] Fase 0 — Entorno y esqueleto del proyecto

### Qué se implementó
- Se verificó el entorno GNU/Linux sobre WSL2 (`Linux`, `gcc 13.3.0`, `GNU Make 4.3`, `pstree 23.7`).
- Se creó la estructura de directorios y archivos base según la sección 5 del plan:
  - `Makefile` con reglas `all`, `$(BIN)/%`, `test` y `clean`, usando `-std=gnu11 -Wall -Wextra -O2 -Iinclude`.
  - `README.md` y `CHECKLIST.md` (con la casilla `F0` marcada).
  - Cabeceras base en `include/`: `common.h`, `span.h`, `protocolo.h`.
  - Archivos fuente esqueleto en `src/`: `common.c`, `collector.c`, `inventario.c`, `api.c`, `launcher.c`.
  - Archivos base para pruebas y herramientas: `tests/test_span.c` y `tools/gen_spans_prueba.c`.
- Se ejecutaron las pruebas de aceptación de la Fase 0 (`make` y `make clean`) sin errores ni advertencias.

---

## [2026-11-09] Fase 1 — Spans, protocolo y utilidades comunes

### Qué se implementó
- Se definió `span_t` en `include/span.h` respetando exactamente los campos del contrato de la sección 3.1 (`trace_id`, `span_id`, `parent`, `servicio`, `operacion`, `ini`, `fin`, `dur`, `estado`, `msg`, `anomalia`, `pid`).
- Se definieron las estructuras de protocolo `req_t`, `ctx_t` y `resp_t` en `include/protocolo.h`.
- Se declararon e implementaron en `include/common.h` y `src/common.c` las funciones auxiliares:
  - `ahora_ns()` con `clock_gettime(CLOCK_MONOTONIC)`.
  - `dormir_ms()` con `nanosleep`.
  - `escribir_exacto()` y `leer_exacto()` para transferencia íntegra de estructuras de tamaño fijo por pipes.
  - `id_hex()` usando `rand()` con semilla por proceso `srand(getpid() ^ time(NULL))` según la alternativa de la sección 0.1.
  - `span_ini()` y `span_fin()` con verificación `assert(sizeof(span_t) <= 512)`.
- Se implementó `tests/test_span.c` verificando longitud y formato hexadecimal de `trace_id` (32) y `span_id` (16), unicidad de IDs consecutivos, propagación de `parent`, medición de duración con `dormir_ms(20)` y lectura/escritura exacta en un pipe anónimo.
- Se actualizó `CHECKLIST.md` marcando `F1` y documentando las decisiones de la sección 0.1.

---

## [2026-11-09] Fase 2 — Colector aislado y generador de spans de prueba

### Qué se implementó
- Se implementó `src/collector.c`:
  - Recibe por `argv[1]` el descriptor de archivo de lectura (`psp[0]`).
  - Lee registros `span_t` con `leer_exacto` hasta recibir EOF (`0`), almacenándolos en un arreglo dinámico redimensionado con `realloc` (duplicando su capacidad).
  - Agrupa los spans por `trace_id` y reconstruye recursivamente el árbol de llamadas (`imprimir_arbol`), identificando como raíz a los spans con `parent` vacío e indentando 3 espacios adicionales por nivel para cada hijo cuyo `parent` coincida con el `span_id` del padre.
  - Soporta de forma segura entrada vacía (`-- 0 trazas, 0 spans`) y spans huérfanos cuyo padre no exista en la traza (imprimiéndolos sin provocar violación de segmento ni recursión infinita).
- Se implementó `tools/gen_spans_prueba.c` para emitir spans binarios por `stdout` y probar el colector mediante una tubería de shell (`./bin/gen | ./bin/collector 0`).
- Se actualizó `CHECKLIST.md` marcando la casilla `F2`.

---

## [2026-11-09] Fase 3 — Servicios aislados (`inventario` y `api`)

### Qué se implementó
- Se implementó `src/inventario.c`:
  - Conserva y utiliza los descriptores `pin[0]` (lectura de `ctx_t`), `pout[1]` (escritura de `resp_t`) y `psp[1]` (escritura de `span_t`).
  - Inicializa el stock según la sección 3.3 (`{10, 10, 10, 10, 5}`).
  - Crea un span hijo heredando `ctx.trace_id` y estableciendo `ctx.parent` como su `parent`.
  - Simula latencia base aleatoria de 10–40 ms con `rand()` y aplica anomalías inyectables (`1` = `+300 ms` `latencia_alta`, `2` = `error_simulado`).
  - Procesa operaciones `consultar_catalogo` y `reservar_stock` (validando `stock_insuficiente`), envía su `span_t` al colector y devuelve `resp_t` a `api`.
- Se implementó `src/api.c`:
  - Conserva y utiliza los descriptores `preq[0]` (lectura de `req_t`), `pin[1]` (escritura de `ctx_t`), `pout[0]` (lectura de `resp_t`) y `psp[1]` (escritura de `span_t`).
  - Crea el span raíz de cada petición (`parent` vacío, genera nuevo `trace_id`), simula 3–9 ms de trabajo propio y resuelve `GET /health` localmente (1 span) o propaga el contexto `ctx_t` (`trace_id` y `parent = span_id`) hacia `inventario` para `GET /products` y `POST /buy`.
  - Ante caída inesperada o EOF prematuro de `inventario`, registra el error en el span raíz, cierra descriptores limpiamente y retorna código `1`.
- Se actualizó `CHECKLIST.md` marcando la casilla `F3`.

---

## [2026-11-09] Fase 4 — Orquestador `launcher` con `fork`/`execv` y cierre selectivo de pipes

### Qué se implementó
- Se implementó `src/launcher.c`:
  - Creación de los 4 pipes anónimos (`preq`, `pin`, `pout`, `psp`) antes de los `fork`.
  - Función `lanzar()` con vaciado previo de buffers (`fflush(stdout)` y `fflush(stderr)`), `fork()`, cierre selectivo de todos los descriptores que cada proceso hijo no debe conservar según la sección 4, paso de descriptores por `argv` y reemplazo de imagen con `execv()` (`_exit(127)` en caso de fallo).
  - Documentación explícita en comentario dentro de `src/launcher.c` de los descriptores conservados y cerrados por cada proceso (Regla 5 de la sección 14).
  - Cierre en el padre de todos los descriptores salvo `preq[1]`, envío de las 6 peticiones de prueba de la sección 3.4, cierre de `preq[1]` para desencadenar la cascada de EOF y recolección de estados de terminación con `waitpid()` (`WIFEXITED`/`WEXITSTATUS` y `WIFSIGNALED`/`WTERMSIG`).
- Se actualizó `CHECKLIST.md` marcando la casilla `F4`.

---

## [2026-11-09] Fase 5 — Pruebas de robustez y evidencia experimental

### Qué se implementó y verificó
- **Ausencia de procesos zombie (`ps -ef --forest`):** Se capturó la jerarquía de procesos en ejecución (`launcher` como padre de `collector`, `inventario` y `api` con sus descriptores en `argv`) y se verificó que tras la finalización no queda ningún proceso `<defunct>`.
- **Árbol de procesos (`pstree -p`):** Se capturó el árbol real durante la ejecución (`launcher(828)-+-api(832) |-collector(830) `-inventario(831)`).
- **Tolerancia a fallo abrupto de un servicio (`kill -9 inventario`):** Se envió `SIGKILL` al proceso `inventario` en mitad de la cuarta petición; el kernel cerró sus extremos de pipe, `api` detectó EOF inesperado en `pout[0]`, cerró su span con `ERROR`, liberó sus descriptores y terminó con código `1`; `collector` recibió EOF en `psp[0]`, imprimió las 4 trazas / 6 spans acumulados hasta el fallo y terminó con código `0`; `launcher` reportó los tres estados (`collector` código 0, `inventario` señal 9, `api` código 1) y retornó código `1` sin bloqueos.
- **Medición de tiempo (`time ./bin/launcher`):** `real 0m0.469s`, `user 0m0.004s`, `sys 0m0.000s`.
- Se actualizó `CHECKLIST.md` marcando la casilla `F5`.

---

## [2026-11-09] Fase 6 — Documentación de entrega, matriz de trazabilidad y verificación final

### Qué se implementó
- Se redactó `README.md` completo con:
  - Instrucciones de compilación (`make`, `make test`, `make clean`) y ejecución (`./bin/launcher` y pruebas aisladas).
  - Diagrama de arquitectura, mapeo conceptual con OpenTelemetry/Tempo/Grafana, tabla de pipes y descriptores por proceso y explicación paso a paso de la cascada de cierre EOF.
  - Decisiones de diseño alineadas con la sección 0.1 del plan.
  - Evidencias reales de ejecución de la Fase 5 (salida de 6 trazas y 11 spans, `ps -ef --forest`, `pstree -p`, fallo inducido con `kill -9 inventario` y medición con `time`).
  - Matriz de trazabilidad verificada contra los requisitos del enunciado.
- Se completó `CHECKLIST.md` marcando la casilla `F6`.
- Se ejecutó la verificación final desde cero (`make clean && make && make test && ./bin/launcher`).

---

## [2026-11-13] Refactorización a Opción 2 y Opción 3 con Sockets UNIX + Pthreads (eliminación de servicios simulados)

### Qué se implementó
- Se eliminaron los servicios simulados y la infraestructura de pipes del MVP anterior (`src/api.c`, `src/inventario.c`, `src/launcher.c`, `src/collector.c`, `include/protocolo.h`, `tools/gen_spans_prueba.c`), manteniendo el núcleo común (`include/span.h`, `include/common.h`, `src/common.c`, `tests/test_span.c`).
- Se implementó **`opcion2_comandos/`** (Opción 2 — Wrapper de comandos existentes del SO):
  - `opcion2_comandos/collector_sock.c`: Colector concurrente mediante Sockets UNIX (`AF_UNIX`, `SOCK_STREAM`) e hilos (`pthread_create` + `pthread_mutex_t`).
  - `opcion2_comandos/wrapper_traza.c`: Selecciona comandos reales del SO (por `argv` o lote predeterminado), lanza `collector_sock` y cada comando con `fork()` + `execvp()`, mide su duración real con `waitpid()` y envía los `span_t` por socket.
  - `opcion2_comandos/Makefile`: Reglas de compilación y ejecución aislada.
- Se implementó **`opcion3_procesos_vivos/`** (Opción 3 — Observación de procesos ya en ejecución por PID):
  - `opcion3_procesos_vivos/collector_sock.c`: Colector concurrente por Socket UNIX (`AF_UNIX`) e hilos (`pthread`).
  - `opcion3_procesos_vivos/monitor_pid.c`: Agente observador lanzado por `fork()` + `execv()` que lee `/proc/<pid>/comm` y `/proc/<pid>/stat` durante una ventana de muestreo y reporta estado y ticks de CPU vía socket.
  - `opcion3_procesos_vivos/orquestador_pids.c`: Selecciona PIDs vivos del SO (por `argv` o escaneando `/proc`), lanza un `monitor_pid` en paralelo por cada PID seleccionado y recoge sus resultados.
  - `opcion3_procesos_vivos/Makefile`: Reglas de compilación y ejecución aislada.
- Se actualizaron `Makefile` (raíz) y `.gitignore` para compilar/limpiar ambas carpetas y excluir sus binarios.

---

## [2026-11-13] Mejora visual en consola: encabezados de columnas, tiempo relativo (`ini`) y cascada estilo Grafana

### Qué se implementó
- Se actualizaron `opcion2_comandos/collector_sock.c` y `opcion3_procesos_vivos/collector_sock.c` para imprimir cada traza en formato de tabla estructurada con:
  - Columna `INICIO(ini)` mostrando el desplazamiento en ms respecto al inicio de la traza (`s->ini - min_ini`).
  - Encabezados explícitos (`INICIO(ini)`, `SERVICIO`, `OPERACION`, `DURACION`, `CASCADA (TIMELINE)`, `PID`, `ESTADO`).
  - Barra visual de cascada (`[#####...............]`) proporcional al tiempo relativo de inicio y duración de cada span, diferenciando ejecución secuencial y paralela.
  - Colores ANSI para distinguir estado `OK` (verde), `ERROR` (rojo) y barras de tiempo.

---

## [2026-11-13] Columnas explícitas de Span ID, Parent ID, Inicio (fecha/hora) y Fin en tabla

### Qué se implementó
- Se actualizaron `opcion2_comandos/collector_sock.c` y `opcion3_procesos_vivos/collector_sock.c` para mostrar de forma explícita todos los campos solicitados:
  - `INICIO` formateado como fecha y hora con milisegundos (`YYYY-MM-DD HH:MM:SS.mmm`) mediante conversión `CLOCK_REALTIME - CLOCK_MONOTONIC` (sin perder la precisión monotónica de `span_t`).
  - `FIN` con hora exacta y milisegundos (`HH:MM:SS.mmm`).
  - `SPAN_ID` (16 caracteres hex) y `PARENT_ID` (`(raiz)` cuando está vacío, o los 16 hex del padre).
  - `SERVICIO`, `OPERACION`, `DURACION`, `CASCADA`, `PID` y `ESTADO`.

---

## [2026-11-13] Opción 3: Descubrimiento recursivo del árbol real de procesos del SO por PPID

### Qué se implementó
- Se actualizó `opcion3_procesos_vivos/orquestador_pids.c` para escanear `/proc/<pid>/stat` leyendo `PID`, `PPID` y `comm`, seleccionar un PID raíz (pasado por `argv[1]` o autoseleccionando el proceso con mayor jerarquía de hijos) y construir recursivamente su subárbol real de descendientes (`construir_subarbol`).
- Se actualizó `opcion3_procesos_vivos/monitor_pid.c` para recibir `<own_span_id>` y `<parent_id>`, preservando en los spans reportados la relación padre-hijo real del árbol de procesos del kernel Linux.
- Se amplió la columna `SERVICIO` en `opcion3_procesos_vivos/collector_sock.c` para visualizar cómodamente múltiples niveles de indentación.

---

## [2026-11-14] Inclusión de `docs/` en el repositorio para contexto en Linux (Lubuntu)

### Qué se implementó
- Se eliminó `docs/` de `.gitignore` para permitir que el historial (`docs/Updates.md`) y el plan (`docs/PLAN_MVP_TRACELITE.md`) formen parte del repositorio remoto.
- De este modo, al clonar el proyecto en Lubuntu, el entorno y Antigravity en Linux contarán con todo el contexto de lo implementado y las pruebas pendientes (observación de árboles de procesos reales como Chrome o Antigravity vía Opción 3).
