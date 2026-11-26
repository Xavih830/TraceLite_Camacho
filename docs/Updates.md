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

---

## [2026-11-14] Opción 3: Modo en tiempo real (`--watch` / `--stream`) con lectura de `/proc/<pid>/cmdline`

### Qué se implementó
- Se actualizó `opcion3_procesos_vivos/collector_sock.c`:
  - Se agregó el flag `--stream` para imprimir en terminal cada `span_t` en tiempo real conforme llega por el Socket UNIX (`fflush(stdout)`), mostrando hora (`HH:MM:SS.mmm`), tipo de evento (`INICIO`, `NUEVO`, `CPU`, `FIN`), PID, proceso, detalle, duración y comando.
  - Se ignoró `SIGINT` en modo `--stream` para que `orquestador_pids` coordine el cierre limpio enviando `__FIN__` y mostrando el resumen final de eventos recibidos.
- Se actualizó `opcion3_procesos_vivos/orquestador_pids.c`:
  - Se añadieron las opciones de línea de comandos `--watch` (`-w`), `--interval <ms>` (por defecto `150 ms`) y `--cycles <n>` manteniendo intacto el modo snapshot original.
  - Se implementó `leer_cmdline_proc()` para leer `/proc/<pid>/cmdline` y capturar el comando completo con argumentos de cada proceso observado.
  - Se ampliaron los límites de observación (`MAX_PROCS_SO = 2048`, `MAX_NODOS = 32`, `MAX_WATCH = 256`) y se añadió manejo de señales (`SIGINT`/`SIGTERM`) para detener el monitoreo limpiamente con `Ctrl+C`.
  - Se implementó `ejecutar_modo_watch()` para detectar y emitir eventos en vivo cuando aparecen nuevos subprocesos (`NUEVO`), cuando consumen CPU (`CPU +N ticks`) y cuando terminan (`FIN` con su duración).
- Se compiló con `make clean && make && make test` sin advertencias y se verificó en Lubuntu observando en vivo el árbol de `antigravity-ide` (`./orquestador_pids 1937 --watch --interval 150 --cycles 10`).

---

## [2026-11-14] Opción 3: Alineación del modo `--watch` (`--stream`) con las columnas completas de trazabilidad

### Qué se implementó
- Se actualizó `imprimir_span_stream()` en `opcion3_procesos_vivos/collector_sock.c` para que el modo en tiempo real (`--stream`) imprima exactamente las mismas columnas requeridas que el modo normal:
  - Encabezado con `TRACE ID`, `Start` y `Modo: WATCH (tiempo real)`.
  - Columnas `INICIO`, `FIN`, `SPAN_ID`, `PARENT_ID`, `SERVICIO`, `OPERACION`, `DURACION`, `CASCADA`, `PID` y `ESTADO` (acompañado del badge de evento `[INICIO]`/`[NUEVO]`/`[CPU]`/`[FIN]` y el comando leído de `/proc/<pid>/cmdline`).
  - Cálculo progresivo de la barra `CASCADA` respecto al inicio del stream y vaciado inmediato de búfer (`fflush(stdout)`).
- Se recompiló con `make && make test` sin advertencias y se verificó con `./orquestador_pids 1937 --watch --interval 150 --cycles 8` (`36 eventos recibidos`).

---

## [2026-11-14] Opción 3: Modo `--resumen` (`--duracion`) para consolidar trazas largas en una tabla jerárquica única

### Qué se implementó
- Se actualizó `opcion3_procesos_vivos/orquestador_pids.c`:
  - Se añadieron los flags `--resumen` (`-r`) y `--duracion <seg>` (`-d <seg>`), manteniendo intactos el modo snapshot original y el modo `--watch`.
  - Se implementó `ejecutar_modo_resumen()`, que monitorea el subárbol del PID raíz en silencio durante la ventana configurada (o hasta `Ctrl+C` / finalización del PID raíz), consolida 1 único registro por cada proceso (`resumen_proc_t`, hasta `MAX_RESUMEN = 512`), acumula sus ticks totales de CPU (`ticks_ult - ticks_ini`), su tiempo real de inicio y fin, su relación padre-hijo y su comando en `/proc/<pid>/cmdline`.
  - Al finalizar la captura, envía los spans consolidados al `collector_sock` normal para imprimir una única tabla jerárquica en árbol sin saturar la terminal.
- Se compiló con `make && make test` sin advertencias y se verificó con:
  - Carga mixta `bash` + `sleep 0.2` + `python3` (`1 trazas, 4 spans`, `python3` con `ticks:+41 (fin)` y `510.52 ms`).
  - `antigravity-ide` (`./orquestador_pids 1937 --resumen --duracion 2 --interval 150`): `1 trazas, 21 spans` consolidados con ticks acumulados por proceso.

---

## [2026-11-14] Opción 3: Corrección de `Ctrl+C` (`SIGINT`) en `--resumen` para que `collector_sock` imprima la tabla

### Qué se implementó
- Se corrigió `opcion3_procesos_vivos/orquestador_pids.c`: al hacer `fork()` para lanzar `collector_sock` en `ejecutar_modo_resumen()` (y en modo snapshot), el proceso hijo ahora ejecuta `signal(SIGINT, SIG_IGN)` antes de `execv()`.
- Con esto, cuando el usuario presiona `Ctrl+C` en la terminal, el kernel no mata prematuramente a `collector_sock` junto con el grupo de procesos en primer plano: `orquestador_pids` captura `SIGINT`, envía todos los spans consolidados y `__FIN__`, y `collector_sock` imprime la tabla completa antes de salir.
- Se recompiló con `make && make test` y se verificó enviando `SIGINT` (`kill -INT`) tras 1 s de captura sobre `antigravity-ide` (`PID 1937`), imprimiendo correctamente `1 trazas, 23 spans`.

---

## [2026-11-15] Opción 3: Mostrar operación/rol real del proceso (`/proc/<pid>/cmdline` y `/proc/<pid>/wchan`)

### Qué se implementó
- Se actualizó `opcion3_procesos_vivos/monitor_pid.c` para reemplazar el texto `estado:S->S ticks:+0` por la operación o rol real que desempeña cada proceso:
  - Lectura de `/proc/<pid>/cmdline` (`extraer_rol_cmdline`) para identificar subprocesos específicos (como `--type=renderer`, `--type=gpu-process`, `--type=utility:...` en apps Electron/Chromium como Antigravity o Chrome, o el comando/argumento real en procesos estándar).
  - Lectura de `/proc/<pid>/wchan` (`leer_wchan`) para mostrar entre corchetes qué está haciendo en el kernel (`[CPU]` si está ejecutando, o la llamada de espera como `[ep_poll]`, `[futex_wait_queue]`, `[do_wait]`).

---

## [2026-11-15] Formato adaptable al ancho de terminal (`TIOCGWINSZ`) y temporizador automático de 60 s en `--resumen`

### Qué se implementó
- Se agregó detección automática del ancho de la terminal (`ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws)` con respaldo en `COLUMNS`) en `opcion2_comandos/collector_sock.c` y `opcion3_procesos_vivos/collector_sock.c`:
  - Ajusta dinámicamente los anchos de `INICIO`, `SPAN_ID`, `PARENT_ID`, `SERVICIO`, `OPERACION`, `CASCADA`, la línea separadora y el recorte de mensajes (`msg`) para que ninguna fila se corte o salte de línea al maximizar o redimensionar la terminal.
- Se actualizó `opcion3_procesos_vivos/orquestador_pids.c`:
  - En modo `--resumen`, la duración predeterminada pasó a **60 segundos** automáticos (sin requerir `Ctrl+C`, manteniendo `-d <seg>` para personalizarla o `-d 0` para modo manual).
  - Se añadió un contador en vivo en la misma línea (`\r[orquestador] Capturando PID ...: X / 60 s`) que muestra los segundos transcurridos y los procesos detectados hasta el cierre automático.

---

## [2026-11-15] Detección estadística de anomalías de tiempo ($\mu + 2\sigma$) y diferenciación explícita de anomalías

### Fórmula implementada para anomalía de tiempo (`latencia_alta`)
Para detectar incrementos aleatorios o picos anómalos en el tiempo de ejecución sin depender de un umbral fijo manual, `src/collector.c` (`evaluar_anomalias_tiempo`) evalúa cada span $i$ con estado `OK` mediante el método estadístico **Leave-One-Out ($\mu_{-i} + 2\sigma_{-i}$)** (excluyendo al propio span $i$ para evitar que un pico extremo contamine su propia línea base en muestras pequeñas):

1. **Conjunto de referencia ($N_{\text{ref}} \ge 2$, equivalente a $\ge 3$ spans comparables):**
   - **Nivel 1 (mismo servicio):** se comparan los demás spans con el mismo `servicio`.
   - **Nivel 2 (traza / lote):** si no hay suficientes spans del mismo servicio, se comparan los demás spans de la misma traza (o del lote completo si la traza tiene $< 3$ spans), excluyendo el span observador `"tracelite"`.
2. **Fórmula:**
   $$\mu_{-i} = \frac{1}{N_{\text{ref}}} \sum_{j \ne i} d_j, \qquad \sigma_{-i} = \sqrt{\frac{1}{N_{\text{ref}}} \sum_{j \ne i} (d_j - \mu_{-i})^2}$$
   $$T_{\text{anom}} = \mu_{-i} + 2\sigma_{-i}$$
3. **Condición de disparo (con filtro anti-jitter del SO):**
   Un span se marca con `anomalia = "latencia_alta"` cuando se cumplen simultáneamente:
   - $d_i > \mu_{-i} + 2\sigma_{-i}$
   - $d_i > 1.5\,\mu_{-i}$ (al menos $50\%$ por encima de la media de referencia)
   - $d_i - \mu_{-i} \ge 10.0\text{ ms}$ (evita falsos positivos por variación mínima de reloj)

### Qué se implementó
- **`src/collector.c` y `Makefile`:**
  - Se enlazó `-lm` en `Makefile` y se implementó `evaluar_anomalias_tiempo()` antes de imprimir las trazas.
  - En la columna `ESTADO` ahora se distingue explícitamente el tipo de anomalía:
    - `ERROR [error_proceso]` (rojo) para fallos de proceso/PID.
    - `ANOMALIA [latencia_alta]` (amarillo, junto con barra amarilla en `CASCADA`) para picos estadísticos de duración.
    - `OK` (verde) para spans normales.
  - La línea final `-- Resumen: ... --` reporta también el conteo de errores y anomalías de tiempo.
- **`src/monitor_pid.c` y `src/tracelite.c`:**
  - Al finalizar un span con error (`pid_no_encontrado`, `no_existe_en_proc`, `proceso_termino`), se rellena explícitamente `span_t.anomalia` con `"error_proceso"`.
- **Verificación:**
  - `make && make test` superado sin advertencias (`sizeof(span_t) = 264 bytes`).
  - Prueba de error (`./bin/tracelite 999999`): `ERROR [error_proceso]` (`1 errores, 0 anomalias de tiempo`).
  - Prueba de latencia variable (`sleep 0.25` x3 + `sleep 2.0` con `--resumen -d 3`): `sleep` de `2113.96 ms` frente a `301.48 ms` marcado como `ANOMALIA [latencia_alta]`.

---

## [2026-11-15] Resolución por nombre de proceso, simplificación de comentarios y limpieza de `--resumen`

### Qué se implementó
- **Resolución automática por nombre (`src/tracelite.c`, `README.md`):**
  - Se añadió `buscar_pid_por_nombre()` y soporte de línea de comandos para pasar un nombre de proceso en lugar del PID (`./bin/tracelite antigravity`, `./bin/tracelite --resumen -d 30 antigravity`, `./bin/tracelite chrome`, `./bin/tracelite postgres`).
  - Se agregó `--help` (`-h`) con ejemplos listos por nombre.
- **Limpieza de la tabla en `--resumen` (`src/tracelite.c`):**
  - Se eliminó la etiqueta `ticks:+N (vivo|fin)` en la columna `OPERACION`, mostrando directamente el comando/operación real leído desde `/proc/<pid>/cmdline`.
- **Refactorización de comentarios:**
  - Se simplificaron los comentarios en `include/*.h`, `src/*.c` y `tests/test_span.c` a guías breves de sección orientadas al programador.

---

## [2026-11-15] Eliminación del modo `--watch` (`--stream`)

### Qué se implementó
- Se eliminó el modo `--watch` (`--stream`) de `src/tracelite.c`, `src/collector.c` y `README.md` (`ejecutar_modo_watch`, `enviar_evento_watch`, `imprimir_span_stream`, flags `-w`/`--watch`/`--cycles`/`--stream`).
- El proyecto queda enfocado en sus dos modos principales: modo instantánea (con `monitor_pid` en paralelo por cada nodo) y modo `--resumen` (consolidado jerárquico con detección de anomalías de tiempo).

---

## [2026-11-15] Unificación de `OPERACION` entre modo instantánea y `--resumen`

### Qué se implementó
- Se actualizó `src/monitor_pid.c` para usar `leer_cmdline_proc()` en lugar de recortar argumentos o añadir sufijos de `wchan`/`[S]`, mostrando exactamente el mismo contenido real de `/proc/<pid>/cmdline` en la columna `OPERACION` que el modo `--resumen`.

---

## [2026-11-15] Actualización de `README.md` (ASD-STE100) y empaquetado de entrega

### Qué se implementó
- Se reescribió `README.md` aplicando un estilo técnico simplificado (~80% ASD-STE100): oraciones breves, voz activa, imperativos directos para compilación/ejecución, modos actualizados (instantánea y `--resumen`), detección de anomalías y referencia a `pruebas/`.

---

## [2026-11-24] Fase F0 — Base y diagnóstico

### Qué se implementó
- Se creó `CHECKLIST.md` con los requisitos `R1`–`R5`, pruebas `P1`–`P3`, `E1`, objetivos `U1`–`U11`, fases `F0`–`F10`/`G1`–`G2` y la tabla de recalibración de servicios del SO.
- Se actualizó `Makefile` para incluir `include/*.h` como dependencia de compilación y agregar los objetivos `ebpf` y `check`.
- Se actualizaron `include/common.h` y `src/common.c`:
  - `id_hex()` ahora usa `getrandom()` (`<sys/random.h>`) con reintento ante `EINTR`.
  - `escribir_exacto()` y `leer_exacto()` reintentan ante `EINTR`.
  - Se implementaron `ruta_hermana()` (basada en `readlink("/proc/self/exe")`), `crear_dir_privado()` (`mkdtemp` en `$XDG_RUNTIME_DIR/tracelite-XXXXXX` o `/tmp/tracelite-XXXXXX`) y `validar_ruta_socket()`.
- Se actualizaron `src/collector.c` y `src/tracelite.c` para usar rutas relativas al binario, socket privado `<dir>/spans.sock` pasado por `argv`, validación de longitud de `sun_path`, manejo de `SIGINT`/`SIGTERM` también en instantánea, espera segura ante `EINTR` (`esperar_hijo`) y limpieza del socket y directorio privado al finalizar.
- Se corrigieron las rutas `pruebas/` → `docs/pruebas/` en `README.md`.
- Se crearon `docs/pruebas/diagnostico_ebpf.sh`, `docs/pruebas/detectar_servicios.sh` y `docs/pruebas/prueba_base.sh`, y se ejecutaron todas las pruebas de aceptación de F0 con éxito.

---

## [2026-11-24] Fase F1 — Núcleo compartido

### Qué se implementó
- Se amplió `span_t` en `include/span.h` con `cpu_ms`, `n_procs` y `atribucion[12]` (288 bytes), y se actualizó `tests/test_span.c` para validar el nuevo tamaño y la inicialización en cero.
- Se crearon `include/protocolo.h` (mensajes `ctx_t`, `resp_t`, `sol_t`, `sol_resp_t`) y `include/fuente.h` (`medida_t` y `fuente_t`).
- Se extrajo la impresión de tablas jerárquicas y resúmenes de `src/collector.c` hacia `include/tabla.h` y `src/tabla.c`, actualizando `Makefile` para enlazar `src/tabla.c` en `bin/collector`.
- Se verificó `make clean && make && make test` (`sizeof(span_t) = 288 bytes`) y la salida del observador sin cambios en columnas.

---

## [2026-11-24] Fase F2 — Solicitudes A/B con `/proc`

### Qué se implementó
- Se añadieron utilidades compartidas de Sockets UNIX con `SOCK_CLOEXEC`/`FD_CLOEXEC` y extracción de nombre base en `include/common.h` y `src/common.c` (`crear_servidor_unix`, `conectar_unix`, `aceptar_unix`, `nombre_base_cmd`).
- Se implementó `src/fuente_proc.c` (`fuente_t` por muestreo cada 20 ms en `/proc` identificando procesos por `PID + starttime` y acumulando `cpu_ms` y `n_procs`).
- Se implementaron `src/servicio_b.c` (`bin/servicio_b`) y `src/servicio_a.c` (`bin/servicio_a`) con barrera de inicio por `pipe2`, `setpgid` para corte por plazo, manejo de `SIGPIPE`, y recuperación ante caída de B (`servicio_b_caido`).
- Se implementó Collector Lite en el proceso principal (`include/solicitudes.h` y `src/solicitudes.c`) y se amplió el parser de `src/tracelite.c` para soportar instantánea, `--resumen`, `-n`, `--`, `--lote`, `--plazo`, `--mostrar-salida` y rechazo con código `2` ante combinaciones inválidas, conservando el modo observación.
- Se creó y superó `tests/test_solicitudes.sh` (7/7 comprobaciones) junto con `make clean && make && make test` sin advertencias y `docs/pruebas/prueba_base.sh`.

---

## [2026-11-24] Fase F3 — Persistencia y resumen

### Qué se implementó
- Se implementó el almacén diario JSONL append-only (`include/almacen.h` y `src/almacen.c`):
  - Segmentos diarios `AAAA-MM-DD.jsonl` en `$TRACELITE_DIR` (o `$HOME/.local/share/tracelite`) con `fflush` y `fsync`, escape JSON completo y sustitución de bytes no UTF-8 por `?`.
  - Índice en memoria (límite 100 000 spans), conteo tolerante de líneas corruptas/truncadas y retención por `--retencion-dias` (7 por defecto) / `--retencion-seg`, eliminando únicamente archivos `AAAA-MM-DD.jsonl` vencidos.
- Se integró la persistencia en `src/solicitudes.c` (`tipo = "solicitud"`) y en `src/collector.c` (`tipo = "observacion"`), y se agregaron `--historial`, `--trace`, `--retencion-dias` y `--retencion-seg` en `src/tracelite.c`.
- Se creó `tests/test_almacen.c` (enlazado a `make test`) y se superaron todas las pruebas de aceptación (`10` líneas válidas en la primera ejecución, `20` en la segunda y reconstrucción por `--historial --trace`).

---

## [2026-11-24] Fase F4 — Anomalías, alertas y pruebas obligatorias

### Qué se implementó
- Se implementó `evaluar_y_alertar_solicitud()` en `src/solicitudes.c` según la sección 4.6:
  - Evalúa cada solicitud completa sobre `servicio_b` tomando hasta 50 referencias previas en `almacen_indice_t` (mismo `servicio`, `operacion`, `atribucion`, `tipo=solicitud`, OK y sin anomalía).
  - Marca `latencia_alta` con `n_ref >= 3` cuando `dur > mu + 2*sigma`, `dur > 1.5*mu` y `dur - mu >= 10 ms`, e imprime la alerta inmediata `[ALERTA] servicio=... trace_id=... tipo=...` (en rojo solo si stdout es TTY) antes de persistir ambos spans en JSONL.
  - Marca `error_proceso` y alerta sobre B (o A si B no existe) cuando `estado != 0`, actualizando el resumen final `-- Resumen: solicitudes=... spans=... alertas=... errores=... --`.
- Se añadió `datos/` a `.gitignore` y se crearon `docs/pruebas/preparar_datos.sh` (calibrado a `normal.bin` = 16 MiB / ~401 ms y `pesado.bin` = 160 MiB), `docs/pruebas/caso_normal.sh` y `docs/pruebas/caso_anomalo.sh`, enlazados a `make check`.
- Se superó `make check` y las 10 repeticiones consecutivas de aceptación (`10/10` con `alertas=0` en `caso_normal.sh` y `10/10` con `alertas=1` en `caso_anomalo.sh`).

---

## [2026-11-25] Fase F5 — eBPF para destino `programa`

### Qué se implementó
- Se generaron las firmas BTF locales y se implementaron `include/ebpf_eventos.h`, `src/bpf/trace.bpf.c` (`tp_btf/sched_process_fork`, `sched_process_exec`, `sched_process_exit`, `sched_switch`, mapas `marcados`, `cpu`, `entrada`, `eventos` `BPF_MAP_TYPE_RINGBUF` y `perdidos`) y `src/fuente_ebpf.c`.
- Se integró `--fuente auto|ebpf|proc` en `src/servicio_b.c` (con verificación de `SO_PEERCRED` y respaldo automático a `/proc` en `auto` cuando faltan capacidades), `src/solicitudes.c` y `src/tracelite.c`.
- Se implementó `make ebpf` en `Makefile` manteniendo `make` funcional sin dependencias eBPF.
- Se creó `docs/pruebas/comparar_fuentes.sh` (50 hijos efímeros): `/proc` (`ventana`) registró `n_procs=1` (`cpu_ms=0.000`), mientras que `eBPF` (`exacta`) registró `n_procs=51` (`cpu_ms=7.542`, `perdidos=0`); también pasaron los casos normal (`alertas=0`) y anómalo (`alertas=1`) con `--fuente ebpf`.

---

## [2026-11-25] Fase F6 — Destino `backend` con `/proc` (nuevo y vivo)

### Qué se implementó
- Se creó `docs/pruebas/backend_demo.py` (`ThreadingHTTPServer` en `127.0.0.1` con rutas `/hash?mb=N`, `/espera?ms=N`, `/error`, `404` y `--mostrar-cabeceras`).
- Se implementaron `resolver_pid_puerto_tcp()` (vía `/proc/net/tcp` y `/proc/*/fd`) y `resolver_pid_unidad()` (`systemctl show -p MainPID`) en `src/common.c`.
- Se añadió el cliente HTTP/1.0 en `src/servicio_a.c` con envío de `traceparent: 00-<trace_id>-<span_id>-01`, lectura hasta EOF, detección de `http_>=400`, `conexion_rechazada` y `plazo_vencido`.
- Se añadió la medición de línea base (`--linea-base`) y resta de `fondo=<ms>` en `src/servicio_b.c`.
- Se integraron `--backend`, `--lanzar-backend`, `--backend-pid`, `--ruta`, `--linea-base` y `--unidad` en `src/solicitudes.c` y `src/tracelite.c`.
- Se creó y superó `tests/test_backend.sh` (4/4: backend nuevo, backend vivo con preservación de PID y `traceparent`, `/error` → `http_500`, y puerto cerrado sin bloqueo).

---

## [2026-11-25] Fase F7 — eBPF por conexión (`backend`)

### Qué se implementó
- Se verificó `tcp_recvmsg` (`vlen=4`: `sk`, `msg`, `len`, `flags`) y los campos de `struct sock_common` (`skc_family`, `skc_daddr`, `skc_rcv_saddr`, `skc_dport`, `skc_num`) en el BTF local del kernel `7.0.0-43-generic`.
- Se añadieron en `src/bpf/trace.bpf.c` los mapas `conexiones`, `hilos_marcados` y `ultima_act`, el programa `SEC("fexit/tcp_recvmsg")` y la propagación por `sched_process_fork` y `sched_switch` por solicitud.
- Se añadió `iniciar_conn` a `include/fuente.h` y se implementó `ebpf_iniciar_conn()` en `src/fuente_ebpf.c` con nivel `atribucion = "conexion"`.
- Se creó y superó `tests/test_ebpf_conexion.sh`:
  - `/espera?ms=300` (`conexion`): `dur_ms=303.55`, `cpu_ms=1.38`.
  - `/hash?mb=64` (`conexion`): `dur_ms=101.39`, `cpu_ms=99.09`.
  - Prueba de ruido con cliente `curl` concurrente: `conexion` mantuvo `cpu_ms=0.49`, frente a `ventana` (`/proc`) con `cpu_ms=220.00`.

---

## [2026-11-25] Fase F8 — Destino `app` (viva y nueva) y servicios existentes del SO

### Qué se implementó
- Se añadió `resolver_pid_nombre_proceso()` en `src/common.c` y soporte completo para destino `app` (`--app NOMBRE|PID`, `--lanzar-app`, `--fin orden|inactividad`) en `src/servicio_a.c`, `src/servicio_b.c`, `src/solicitudes.c` y `src/tracelite.c`.
- Se implementó la detección de reposo en `src/servicio_b.c` (3 ventanas seguidas de 100 ms con delta de CPU `<= tasa_base * 100 ms + 2 ms`, anotando `sin_reposo` si vence el plazo).
- Se creó y ejecutó `docs/pruebas/demo_servicios_so.sh`:
  - `cups.service` (`PID 7280`): `5` solicitudes (`GET /`), `10` spans, `0` errores.
  - `systemd-resolved.service` (`PID 788`): `5` solicitudes (`resolvectl query localhost`), `10` spans, `0` errores.
- Se verificó también `app` viva preservando su PID con atribución `ventana` y `fondo=` en JSONL.

---

## [2026-11-25] Fase F9 — Modos, niveles de tiempo y lotes

### Qué se implementó
- Se añadieron los modos `--modo respuesta|estabilidad|escalado|fallo` y los niveles `--tiempo normal|alto|muy-alto` en `include/solicitudes.h`, `src/solicitudes.c` y `src/tracelite.c`:
  - `respuesta`: `--slo-ms` (por defecto 200 ms en `app`, 1000 ms en `programa`/`backend`), alerta `slo_excedido` y clase de latencia (`instantanea`, `fluida`, `atencion`, `excedida`) en `msg` de A.
  - `estabilidad`: calibración previa no persistida, evaluación al cierre del lote por `modo`, `destino` y `atribucion`.
  - `escalado`: `--entradas`, `--tamanos` (por defecto `1,2,4,8,16`) y detección de `escalado_no_lineal` (`ratio > 1.5 * mediana` con diferencia `>= 10 ms`).
  - `fallo`: `--fallo-en N` con inyección de entrada inválida de lectura (`error_proceso`).
- Se creó `docs/pruebas/matriz_modos.sh` y el objetivo `make check-modos` en `Makefile`, superando los 5 casos (observando en equipo de 16 núcleos que tamaños muy pequeños como `mb=8`/`mb=16` muestran varianza inicial frente a la sobrecarga fija de ~20 ms, por lo que `matriz_modos.sh` usa `mb=64` en estabilidad y `mb=32..512` en escalado sin alterar los umbrales).

---

## [2026-11-25] Fase F10 — Documentación final

### Qué se implementó
- Se verificaron [S3] (`BatchSpanProcessor` `scheduleDelayMillis = 5000 ms` en OpenTelemetry) y [S7] (`traceparent` en W3C Trace Context).
- Se actualizó `README.md` con instrucciones de compilación (`make`, `make ebpf`), `setcap`, formas, destinos, modos, niveles de tiempo, `span_t` (288 bytes) y limitaciones.
- Se crearon `docs/diagramas/arquitectura_ipc.md`, `docs/INFORME_FINAL_TRACELITE.md` y `docs/GUION_VIDEO.md`, y se completó `CHECKLIST.md` marcando `F10` y `E1`.


---

## [2026-11-25] Fase G1 — Interfaz gráfica base (`tracelite-gui`)

### Qué se implementó
- Se agregó el objetivo `gui` en `Makefile` usando `pkg-config --cflags --libs gtk4`, manteniendo `make` independiente de GTK.
- Se creó `src/gui.c` con el visor GTK 4 + Cairo de solo lectura (`almacen_cargar` con retención `0, 0`), monitoreo en vivo con `GFileMonitor`, filtros (texto/Trace ID, estado, modo, solo alertas), cascada de spans en Cairo, detalle de span con significado de `atribucion` y pestaña de alertas.
- Se verificó compilación sin advertencias (`-Wall -Wextra`) y tolerancia a directorio vacío, líneas JSONL truncadas/corruptas y recarga en vivo sin modificar los archivos.


---

## [2026-11-25] Fase G2 — Interfaz gráfica completa (`tracelite-gui`)

### Qué se implementó
- Se creó `include/gui_form.h` y `src/gui_form.c` con la tabla estática en C (`TABLA_CAMPOS`) de controles `GtkDropDown`, `GtkSpinButton` y `GtkEntry`, visibilidad dinámica según forma (`Observación`, `Instantánea`, `Resumen`), destino (`programa`, `backend`, `app`), modo (`respuesta`, `estabilidad`, `escalado`, `fallo`) y tiempo (`normal`, `alto`, `muy-alto`, oculto en observación).
- Se implementó `gui_form_construir_argv` y `gui_form_formatear_preview` para construir un arreglo literal de argumentos (`argv[]`, nunca shell) y mostrar el comando resultante en vivo.
- Se integró el formulario en `src/gui.c` mediante `GSubprocess` (`g_subprocess_newv`), lectura asíncrona de salida estándar/error en un `GtkTextView`, botón **Detener** (`SIGINT` solo al `tracelite` lanzado por la interfaz), bloqueo de ejecuciones simultáneas, diálogo de confirmación al cerrar durante una ejecución y tabla de procesos en el detalle para `tipo = observacion`.
- Se verificó compilación sin advertencias (`make && make test && make gui`), prueba unitaria de `gui_form_construir_argv`, paso literal de `; touch /tmp/tl_g2_inject` sin inyección de shell, preservación de PID de proceso vivo tras `SIGINT` a `tracelite` y arranque limpio de `bin/tracelite-gui`.


---

## [2026-11-25] Mejoras finales — Árbol vivo en `--app`, columna `RELACION`, ventana `-d`, contador por solicitud y guía de pruebas

### Qué se implementó
- Se corrigió `ejecutar_modo_resumen` en `src/tracelite.c` para enviar al colector todos los spans capturados en `hist[]` (en lugar de solo el span raíz `resumen_arbol_so`).
- Se mejoró `dibujar_cascada` en `src/gui.c` con `GtkScrolledWindow` de altura dinámica, orden jerárquico DFS y margen derecho ampliado para evitar recortes.
- Se agregó `tabla_formatear_relacion` y la columna/campo `RELACION` en `include/tabla.h`, `src/tabla.c` y `src/gui.c` para identificar el rol real de cada span (`lanzador -> ...`, `sensor_so -> ...`, `cliente_http -> backend`, `hijo_so -> ...`).
- Se implementó muestreo continuo del árbol vivo en `src/servicio_b.c` (`hilo_muestreo_app` + `emitir_arbol_app_muestreado`) para destino `--app`, junto con el soporte de ventana `-d <seg>` por solicitud (`TRACELITE_VENTANA_SEG` en `include/solicitudes.h`, `src/solicitudes.c`, `src/servicio_a.c`, `src/servicio_b.c` y `src/tracelite.c`).
- Se agregó en `src/solicitudes.c` el contador en vivo por solicitud (`[tracelite] Solicitud X / N: S / M s...`) y en `src/tabla.c` / `src/gui.c` la etiqueta `Solicitud N` por traza.
- Se eliminaron los presets `--tiempo` (`normal`, `alto`, `muy-alto`) del formulario gráfico (`include/gui_form.h`, `src/gui_form.c`, `src/gui.c`), dejando la configuración manual de `Duración (s)` (`-d`) y `Plazo máx (s)` (`--plazo`).
- Se crearon `docs/pruebas/pruebas_vivos.sh` y `docs/GUIA_PRUEBAS_ANTIGRAVITY.md`.