# PLAN MVP — TraceLite (Proyecto Parcial, Sistemas Operativos, Avance 1)

Trazabilidad distribuida liviana en C sobre GNU/Linux, con `fork`/`exec`, pipes, spans y un colector que muestra las trazas en terminal.

> **Cómo usar este documento.** Está escrito para ejecutarse **una fase a la vez**. Al modelo que lo ejecute: lee primero las secciones 0 a 4 (contexto y contratos), luego haz **solo la fase que se te indique**, corre sus pruebas de aceptación, marca la casilla en `CHECKLIST.md` y **detente**. No adelantes fases, no agregues funciones que no estén aquí, no cambies los contratos de la sección 3 sin avisar.

---

## 0. Prerrequisito crítico: GNU/Linux

El enunciado exige GNU/Linux. `fork`, `exec`, `pipe` y `waitpid` **no existen** en Windows/MinGW y en macOS hay diferencias (por ejemplo, no hay `pstree` por defecto). Antes de la Fase 0 verifica:

```bash
uname -s          # debe imprimir: Linux
gcc --version     # >= 9
make --version
pstree --version  # sudo apt install psmisc  (si falta)
```

Si trabajas en Windows: **WSL2 con Ubuntu** (o una VM, o el laboratorio de la ESPOL). Trabaja dentro del sistema de archivos de Linux (`~/tracelite`), no en `/mnt/c/...`, porque ahí la compilación es lenta y los permisos se alteran.

## 0.1 Límites de conocimiento (obligatorio respetar)

El proyecto debe poder defenderse solo con lo visto en el curso (capítulos 1 a 4: introducción, estructuras del SO, procesos e IPC, hilos). Todo lo que no esté en la lista blanca **no se usa**, aunque sea mejor técnicamente. Ante la duda, el agente se detiene y pregunta en lugar de implementar.

**Lista blanca (se puede usar sin consultar):** `fork`, `execv`, `waitpid`, `exit`/`_exit`, `getpid`, `kill`, `pipe`, `read`, `write`, `close`, `open` de archivos normales, `clock_gettime`, `nanosleep`, `malloc`/`realloc`/`free`, `snprintf`, `strcmp`, `memset`, structs de tamaño fijo, recursión, `Makefile`, `pstree`/`ps`/`time` desde la terminal.

**Lista de confirmación (Xavier debe validar antes de que se use; mientras tanto, usar la alternativa):**

| Elemento | Alternativa dentro de lo visto |
|----------|--------------------------------|
| `signal(SIGPIPE, SIG_IGN)` y `PIPE_BUF` | Explicar la atomicidad de escrituras pequeñas solo si el profesor la mencionó; si no, omitir el `_Static_assert` y mantener `span_t` por debajo de 512 bytes |
| `/dev/urandom` y `rand_r` | `rand()` con `srand(getpid() ^ time(NULL))` en cada proceso |
| `_Static_assert` | `assert(sizeof(span_t) <= 512)` en runtime |
| `valgrind`, `getrusage` | No usarlos |

**Lista negra (prohibido en el MVP):** hilos (`pthread`), mutex, semáforos, variables de condición, memoria compartida, sockets, `select`/`poll`/`epoll`, señales personalizadas, pipes con nombre (FIFO), `dup2` (salvo que se confirme), bibliotecas externas, cualquier técnica de la sección 12.

**Regla de decisión:** si para implementar un paso hace falta un concepto que no está en las listas anteriores, el agente no lo implementa; escribe en `CHECKLIST.md` una nota con el concepto, por qué haría falta y qué alternativa más simple existe, y continúa con la alternativa.

## 1. Alcance del MVP

**Dentro (obligatorio para el avance):**

| # | Requisito | Cómo se cubre |
|---|-----------|---------------|
| R1 | Varios procesos independientes | 4 procesos: `launcher`, `collector`, `api`, `inventario` |
| R2 | Creación con `fork`/`exec` | El launcher hace `fork()` + `execv()` por cada servicio |
| R3 | Comunicación entre procesos | 4 pipes anónimos (sección 4) |
| R4 | Trace ID / Span ID / Parent Span ID | `span_t` (sección 3) y propagación en `ctx_t` |
| R5 | Medición de tiempos | `clock_gettime(CLOCK_MONOTONIC)` |
| R6 | Colector que recibe spans | Proceso `collector` lee `psp[0]` |
| R7 | Visualización en terminal | Árbol indentado por traza con duraciones |
| R8 | Cierre limpio sin zombies ni colgados | Cascada de EOF + `waitpid` |

**Valor agregado incluido en el MVP (barato, pero visible en la defensa):** estado OK/ERROR por span, anomalías inyectables (`latencia_alta`, `error_simulado`) marcadas en la salida, y PID del proceso en cada span.

**Fuera del MVP** (ver sección 12, solo si sobra tiempo): tiempo propio (self time), cascada visual de barras, getrusage/CPU, servicios de nivel 2 y 3, colector multihilo, memoria compartida.

## 2. Arquitectura

```
                          +------------------+
   (peticiones) preq      |      api         |   pin    +----------------+
launcher ----------------->| span raíz        |--------->|  inventario    |
                          | genera trace_id  |<---------| span hijo      |
                          +--------+---------+   pout   +--------+-------+
                                   | psp                         | psp
                                   v                             v
                              +------------------------------------+
                              |            collector               |
                              |  lee spans, arma árbol, imprime    |
                              +------------------------------------+
```

- **launcher** (padre): crea pipes, hace fork+exec de los otros tres, envía 6 peticiones de prueba por `preq`, cierra `preq[1]` (señal de "no hay más trabajo") y hace `waitpid`.
- **api**: recibe una petición, crea el **span raíz** (sin padre), invoca a `inventario` pasando contexto (`ctx_t`) y espera la respuesta. Cierra su span y lo envía al colector.
- **inventario**: recibe `ctx_t`, crea un **span hijo** con el `trace_id` y `parent` recibidos, simula trabajo, responde `resp_t`, envía su span al colector.
- **collector**: lee `span_t` hasta EOF, y al final agrupa por `trace_id` e imprime el árbol.

Mapeo con lo visto en clase: instrumentación = código de spans en `api`/`inventario`; Collector de OpenTelemetry = `collector`; Tempo = el arreglo en memoria del colector; Grafana = la salida en terminal.

**Por qué pipes** (para la defensa): son canales unidireccionales gestionados por el kernel con buffer acotado (modelo productor-consumidor con bloqueo); toda escritura de hasta `PIPE_BUF` (4096 bytes en Linux) es **atómica**, por lo que varios procesos pueden escribir spans en el mismo pipe sin mezclarse. Se aprovecha la herencia de descriptores entre `fork` y `exec`.

## 3. Contratos (no modificar)

### 3.1 Estructuras (`include/span.h`, `include/protocolo.h`)

```c
typedef struct {
    char    trace_id[33];       /* 32 hex + '\0' */
    char    span_id[17];        /* 16 hex + '\0' */
    char    parent[17];         /* "" si es raíz */
    char    servicio[24];
    char    operacion[48];
    int64_t ini;                /* ns, CLOCK_MONOTONIC */
    int64_t fin;                /* ns */
    double  dur;                /* ms */
    int     estado;             /* 0 = OK, 1 = ERROR */
    char    msg[64];            /* mensaje de error o "" */
    char    anomalia[24];       /* "", "latencia_alta", "error_simulado" */
    int     pid;
} span_t;
_Static_assert(sizeof(span_t) <= PIPE_BUF, "span_t debe caber en PIPE_BUF");

typedef struct { int id; char ruta[24]; int producto; int cantidad; int anomalia; } req_t;
typedef struct { char trace_id[33]; char parent[17]; char op[24];
                 int producto; int cantidad; int anomalia; } ctx_t;   /* contexto propagado */
typedef struct { int estado; char msg[64]; } resp_t;
```

`anomalia` en `req_t`/`ctx_t`: `0` = ninguna, `1` = latencia alta (+300 ms), `2` = error simulado.

Todos los mensajes por pipe son **structs de tamaño fijo**: se lee y escribe siempre `sizeof(struct)` bytes. Esto evita parsear texto y hace atómicas las escrituras.

### 3.2 Funciones auxiliares (`common.c`)

`ahora_ns()`, `dormir_ms()`, `escribir_exacto(fd,buf,n)`, `leer_exacto(fd,buf,n)` (devuelve `1` ok, `0` EOF limpio, `-1` error o lectura parcial), `id_hex(out, bytes)` (lee `/dev/urandom`), `span_ini(...)`, `span_fin(...)`. Existe una implementación de referencia **ya probada** en la sección 13; úsala tal cual.

### 3.3 Reglas del servicio de inventario

- Stock inicial: productos 0–3 con 10 unidades, producto 4 con 5.
- `consultar`: sin cambios de stock. `reservar`: descuenta; si `cantidad > stock` o producto fuera de 0–4 → `estado=1`, `msg="stock_insuficiente"`.
- Latencia base de inventario: 10–40 ms aleatorios (`rand_r`, nunca `rand`).
- La `api` hace 3–9 ms de trabajo propio antes de llamar a `inventario`.

### 3.4 Peticiones de prueba del launcher

| id | ruta | producto | cantidad | anomalia | Resultado esperado |
|----|------|----------|----------|----------|--------------------|
| 1 | `GET /products` | 0 | 1 | 0 | OK, 2 spans |
| 2 | `GET /health` | 0 | 0 | 0 | OK, **1 span** (no llama a inventario) |
| 3 | `POST /buy` | 1 | 1 | 0 | OK, 2 spans |
| 4 | `POST /buy` | 2 | 1 | 1 | OK pero ~300 ms más lenta, marcada anomalía |
| 5 | `POST /buy` | 3 | 1 | 2 | ERROR en ambos spans |
| 6 | `POST /buy` | 4 | 999 | 0 | ERROR por `stock_insuficiente` |

Total esperado: **6 trazas, 11 spans**.

## 4. Pipes y descriptores (la parte que más falla)

Cuatro pipes creados por el launcher **antes** de cualquier `fork`:

| Pipe | Sentido | Lector | Escritor | Contenido |
|------|---------|--------|----------|-----------|
| `preq` | launcher → api | api | launcher | `req_t` |
| `pin` | api → inventario | inventario | api | `ctx_t` |
| `pout` | inventario → api | api | inventario | `resp_t` |
| `psp` | todos → collector | collector | api, inventario | `span_t` |

**Conjuntos de descriptores que cada hijo conserva (todo lo demás se cierra antes del `exec`):**

| Proceso | Conserva |
|---------|----------|
| collector | `psp[0]` |
| inventario | `pin[0]`, `pout[1]`, `psp[1]` |
| api | `preq[0]`, `pin[1]`, `pout[0]`, `psp[1]` |
| launcher (tras los 3 forks) | solo `preq[1]` |

**Regla de oro:** un lector recibe EOF únicamente cuando **todos** los descriptores de escritura del pipe están cerrados. Un solo `close` olvidado en cualquier proceso y el programa se queda colgado para siempre.

**Cascada de cierre esperada:** el launcher cierra `preq[1]` → `api` lee EOF y termina → se cierran `pin[1]` y su `psp[1]` → `inventario` lee EOF y termina → se cierra su `psp[1]` → `collector` lee EOF, imprime y termina.

Los descriptores se pasan al servicio como **argumentos de `argv`** (por ejemplo `./bin/api 3 6 5 8`), porque tras `exec` el programa nuevo conserva los fds abiertos pero ya no conoce sus números.

Al inicio de cada proceso: `signal(SIGPIPE, SIG_IGN)` para que una escritura a un pipe roto devuelva `EPIPE` en vez de matar al proceso sin mensaje.

## 5. Estructura del proyecto

```
tracelite/
├── Makefile
├── README.md
├── CHECKLIST.md
├── include/  common.h  span.h  protocolo.h
├── src/      common.c  collector.c  inventario.c  api.c  launcher.c
├── tools/    gen_spans_prueba.c        (solo Fase 2)
├── tests/    test_span.c               (solo Fase 1)
└── bin/
```

`Makefile` (¡las líneas de receta empiezan con **TAB**, no con espacios!):

```make
CC      = gcc
CFLAGS  = -std=gnu11 -Wall -Wextra -O2 -Iinclude
BIN     = bin
COMUN   = src/common.c
all: $(BIN)/launcher $(BIN)/collector $(BIN)/api $(BIN)/inventario
$(BIN)/%: src/%.c $(COMUN)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) $< $(COMUN) -o $@
test: all
	$(CC) $(CFLAGS) tests/test_span.c $(COMUN) -o $(BIN)/test_span && ./$(BIN)/test_span
clean:
	rm -rf $(BIN)
.PHONY: all test clean
```

No definas `_GNU_SOURCE` dentro de los `.c` si ya usas `-std=gnu11`; si lo necesitas, ponlo en un solo lugar (`common.h`, con `#ifndef _GNU_SOURCE`) para evitar la advertencia de redefinición.

## 6. Fases

Cada fase termina con **pruebas de aceptación**. Si una falla, se corrige antes de continuar.

### Fase 0 — Entorno y esqueleto (≈ 30 min)
1. Verificar la sección 0. 2. Crear el árbol de la sección 5 con archivos vacíos y el `Makefile`. 3. `git init` y primer commit.

**Aceptación:** `make` compila sin errores (aunque no haga nada) y `make clean` funciona.

### Fase 1 — Spans y utilidades (≈ 1 h)
Implementar `span.h`, `protocolo.h`, `common.c`: tiempo, IDs aleatorios, `span_ini`/`span_fin`, E/S exacta. Escribir `tests/test_span.c` con `assert`:
- `trace_id` tiene 32 caracteres hexadecimales, `span_id` 16.
- Dos `span_id` consecutivos son distintos.
- `span_fin` deja `fin > ini` y `dur` coherente tras `dormir_ms(20)` (entre 19 y 60 ms).
- Un span raíz tiene `parent` vacío; uno hijo copia el del contexto.

**Aceptación:** `make test` imprime éxito. Sin warnings con `-Wall -Wextra`.

### Fase 2 — Colector aislado (≈ 1 h)
Implementar `collector.c` leyendo de un fd recibido por argumento. Crear `tools/gen_spans_prueba.c`, que escribe 2 trazas fabricadas en `stdout` (fd 1) y se usa con una tubería de shell: `./bin/gen | ./bin/collector 0`.

Algoritmo del colector: leer `span_t` con `leer_exacto` hasta EOF → guardar en un arreglo dinámico (`realloc` duplicando capacidad) → para cada `trace_id` distinto, imprimir recursivamente: raíz = span con `parent==""`, hijos = spans cuyo `parent` sea el `span_id` del padre, indentando 3 espacios por nivel.

Formato de salida de referencia (ejemplo de formato, **no** son resultados reales):

```
TRACE e5c0e2b25821583405d9ba83a77a5da8
  api        POST /buy             27.41 ms  pid 169  OK
     `- inventario reservar_stock        24.15 ms  pid 168  OK
```

**Aceptación:** con las trazas fabricadas se ve el árbol correcto; con entrada vacía imprime `0 trazas, 0 spans` y termina; con un span cuyo padre no existe, el programa no se cae (se imprime como huérfano o se ignora, pero sin segfault).

### Fase 3 — Servicios aislados (≈ 1.5 h)
Implementar `inventario.c` y `api.c` según las secciones 3 y 4. Probarlos **sin launcher** conectando pipes con un programa de prueba corto o directamente con la Fase 4 en modo parcial. Si se prefiere, saltar a la Fase 4 y probar todo junto: el prototipo de la sección 13 demuestra que el diseño completo funciona.

**Aceptación:** una petición `POST /buy` produce dos spans con el mismo `trace_id`, y el de `inventario` tiene como `parent` el `span_id` del de `api`.

### Fase 4 — Launcher con fork/exec (≈ 1.5 h)
Implementar `launcher.c`:
1. Crear los 4 pipes. 2. `lanzar()` ×3 (orden: collector, inventario, api) con el cierre selectivo de la sección 4 antes de `execv`. 3. En el padre, cerrar todos los fds salvo `preq[1]`. 4. Escribir las 6 peticiones. 5. `close(preq[1])`. 6. `waitpid` de los tres hijos revisando `WIFEXITED`/`WEXITSTATUS`.

Detalles: `fflush(stdout)` **antes** de cada `fork` (si no, el buffer pendiente se duplica y se imprime dos veces); tras un `execv` fallido, `_exit(127)` y no `exit`.

**Aceptación:** ejecutar `./bin/launcher` imprime 6 trazas y 11 spans, los tres `[launcher] ... terminó con código 0`, y devuelve `echo $?` = 0. Ver sección 7.

### Fase 5 — Robustez y evidencia (≈ 1.5 h)
1. **Sin zombies**: ejecutar `./bin/launcher & sleep 0.1; ps -ef --forest | grep -E 'launcher|api|inventario|collector'` durante la ejecución y comprobar que, al terminar, no queda `<defunct>`.
2. **Árbol de procesos** para el informe: mientras corre (añade temporalmente `dormir_ms(3000)` en `api` o usa una petición lenta), ejecutar `pstree -p $(pgrep launcher)` y capturar la pantalla.
3. **Fallo de un hijo**: matar a `inventario` con `kill -9` en mitad de la ejecución; el sistema debe terminar sin colgarse (la `api` recibe EOF en `pout`, devuelve 1; el launcher lo reporta). Documentar el comportamiento.
4. **Tiempo de ejecución** con `time ./bin/launcher`.
5. `valgrind --leak-check=full ./bin/collector 0 < /dev/null` sin fugas (si valgrind está disponible).

**Aceptación:** los cinco puntos documentados con capturas o texto.

### Fase 6 — Entrega (≈ 1 h)
`README.md` (compilar, ejecutar, arquitectura, decisiones), diagrama de arquitectura, capturas, `CHECKLIST.md` completo, mensajes de `git log` razonables. Revisar el enunciado una última vez contra la matriz de la sección 10.

## 7. Salida de referencia de la ejecución completa

Formato esperado (los IDs, PIDs y milisegundos **cambiarán** en cada corrida; no copiar estos números como resultado propio):

```
TRACE <32 hex>
  api        GET /products         ~24 ms  pid P1  OK
     `- inventario consultar_catalogo   ~21 ms  pid P2  OK
TRACE <32 hex>
  api        GET /health            ~4 ms  pid P1  OK
TRACE <32 hex>
  api        POST /buy             ~27 ms  pid P1  OK
     `- inventario reservar_stock       ~24 ms  pid P2  OK
TRACE <32 hex>
  api        POST /buy            ~316 ms  pid P1  OK
     `- inventario reservar_stock      ~310 ms  pid P2  OK  <-- anomalia
TRACE <32 hex>
  api        POST /buy             ~27 ms  pid P1  ERROR
     `- inventario reservar_stock       ~24 ms  pid P2  ERROR  <-- anomalia
TRACE <32 hex>
  api        POST /buy             ~25 ms  pid P1  ERROR
     `- inventario reservar_stock       ~18 ms  pid P2  ERROR
-- 6 trazas, 11 spans
```

Invariantes que **sí** deben cumplirse siempre: 6 trazas / 11 spans; la traza 2 tiene un solo span; la traza 4 es la más lenta (> 300 ms); las trazas 5 y 6 terminan en ERROR; la duración del span `api` es **mayor o igual** que la de su hijo `inventario`.

## 8. Errores típicos (y cómo reconocerlos)

| Síntoma | Causa probable |
|---------|----------------|
| El programa **se cuelga** al final | Un descriptor de escritura quedó abierto en algún proceso (revisar los conjuntos de la sección 4; el launcher debe cerrar todo salvo `preq[1]`, y luego cerrar `preq[1]`) |
| Salida impresa dos veces | Falta `fflush(stdout)` antes de `fork` |
| `execv: No such file or directory` | Se ejecuta desde un directorio distinto; usar rutas relativas a la raíz del proyecto o `argv[0]` |
| Colector imprime menos spans de los esperados | Lectura parcial o `leer_exacto` mal escrito; comprobar el tamaño de cada lectura |
| Programa muere en silencio | `SIGPIPE`: falta `signal(SIGPIPE, SIG_IGN)` |
| Duraciones negativas o enormes | Mezclar `CLOCK_REALTIME` con `CLOCK_MONOTONIC`, o dividir mal los ns |
| Los hijos comparten números aleatorios | Usar `rand()` con la misma semilla; usar `rand_r` con semilla distinta por PID |
| Trazas mezcladas entre sí | `trace_id` no se propaga en `ctx_t`, o se genera uno nuevo en `inventario` |
| `Makefile:3: *** missing separator` | Se usaron espacios en lugar de TAB |
| Corrupción de datos en el pipe | Estructuras con padding distinto entre procesos: compilar todos con el mismo header y flags |

## 9. Preguntas probables en la defensa

- **¿Por qué pipes y no memoria compartida?** Los pipes dan sincronización gratis (el kernel bloquea al lector si está vacío y al escritor si está lleno); la memoria compartida exigiría semáforos o mutex propios. La compensación es una copia extra de datos.
- **¿Qué garantiza que dos servicios no mezclen sus spans en el mismo pipe?** Escrituras ≤ `PIPE_BUF` son atómicas por POSIX; `span_t` lo verifica con `_Static_assert`.
- **¿Qué pasa si un servicio muere?** Se cierran sus descriptores, los lectores reciben EOF o los escritores reciben `EPIPE`; no hay interbloqueo.
- **¿Qué es un span raíz?** El span sin padre donde entra la petición al sistema; no es "un servicio", es el primer span de la traza.
- **¿Por qué `CLOCK_MONOTONIC`?** No retrocede con cambios de hora y es comparable entre procesos de la misma máquina.
- **¿Qué diferencia hay entre `fork` y `exec`?** `fork` duplica el proceso; `exec` reemplaza su imagen. Separarlos permite ajustar los descriptores del hijo entre ambos.
- **¿Cómo se evitan los zombies?** El padre llama `waitpid` para cada hijo.
- **¿Cómo se relaciona con OpenTelemetry/Tempo/Grafana?** Ver sección 2: mismo modelo conceptual a escala de laboratorio.

## 10. Matriz de trazabilidad con el enunciado

| Requisito del enunciado | Dónde se cumple | Evidencia |
|-------------------------|-----------------|-----------|
| GNU/Linux | Sección 0 | `uname -s` |
| Procesos con `fork`/`exec` | `launcher.c` | `pstree` (Fase 5) |
| IPC | 4 pipes | Salida del launcher |
| Trace/Span/Parent ID | `span_t`, `ctx_t` | Árbol correcto |
| Medición de tiempos | `span_ini/fin` | Duraciones |
| Colector | `collector.c` | 6 trazas, 11 spans |
| Visualización en terminal | `imprimir_arbol` | Sección 7 |
| Manejo de errores | `leer_exacto`, `waitpid` | Fase 5 |
| Makefile, README | Fase 6 | Archivos |

*(Contrastar siempre con el PDF del enunciado: si éste exige algo no listado, añadirlo como fila nueva y tratarlo como obligatorio.)*

## 11. CHECKLIST.md (plantilla)

```markdown
- [ ] F0 Linux verificado, esqueleto y Makefile compilan
- [ ] F1 span/utilidades + `make test` verde
- [ ] F2 colector imprime árbol con datos fabricados
- [ ] F3 api e inventario propagan trace_id/parent
- [ ] F4 launcher: 6 trazas / 11 spans, exit 0
- [ ] F5 sin zombies, pstree, kill -9, time, valgrind
- [ ] F6 README, diagrama, capturas, matriz revisada
```

## 12. Extensiones (solo después de terminar la Fase 6)

Ordenadas por relación valor/esfuerzo: (1) **tiempo propio** de cada span = duración − suma de hijos secuenciales; (2) **cascada de barras** `█` proporcional a la duración dentro de cada traza; (3) **CPU time** con `getrusage` o `clock_gettime(CLOCK_PROCESS_CPUTIME_ID)` para distinguir espera de cómputo; (4) tercer servicio (`pagos`) para trazas de 3 niveles; (5) ráfagas de N peticiones con varios clientes concurrentes; (6) colector multihilo con mutex y variable de condición (productor-consumidor del curso); (7) memoria compartida como IPC alternativo con comparación de rendimiento.

## 13. Implementación de referencia validada

Se escribió y ejecutó un prototipo desechable de un solo archivo que usa exactamente este diseño (4 pipes, cierre selectivo, `execv` del propio binario con el rol en `argv`, cascada de EOF, E/S de structs de tamaño fijo). Resultado verificado en Linux: **6 trazas, 11 spans**, los tres procesos terminan con código 0 y no queda ningún descriptor ni proceso colgado. El prototipo ejecuta los tres roles en un solo ejecutable por simplicidad; el proyecto real los separa en cuatro binarios, pero la lógica de cada función es la misma. Ese archivo se entrega junto a este plan como `proto.c` (referencia opcional, no es el entregable). Si está disponible, el modelo ejecutor puede consultarlo para copiar la lógica de cada función al `src/` correspondiente; si no, debe implementar desde las secciones 3 y 4.

Dos advertencias que dejó la compilación del prototipo y que el plan ya incorpora: no redefinir `_GNU_SOURCE` si ya va en los flags, y mantener los mensajes de error compuestos (`"inventario: %s"`) dentro del tamaño de `msg[64]` (usar un prefijo corto o un `%.40s`).

## 14. Reglas para el modelo que ejecute este plan

0. Respetar la sección 0.1 antes que cualquier otra sección: si una sección posterior sugiere algo de la lista de confirmación o de la lista negra, prevalece la 0.1.
1. Una fase por vez; al terminar, correr las pruebas de aceptación y mostrar su salida real.
2. No inventar resultados: si algo no se pudo ejecutar, decirlo.
3. No cambiar nombres de structs, campos ni orden de fds sin avisar.
4. Compilar siempre con `-Wall -Wextra`; los warnings se corrigen, no se ignoran.
5. Revisar cada `close`: antes de dar por buena la Fase 4, escribir en un comentario qué fds conserva cada proceso y compararlo con la sección 4.
6. Ante un cuelgue, **no** agregar `sleep` ni `kill`: revisar descriptores abiertos con `ls -l /proc/<pid>/fd`.
7. Código y comentarios en español, nombres coherentes con este documento.
8. Antes de terminar: `make clean && make && ./bin/launcher` desde cero.
