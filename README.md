# TraceLite — Sistema Liviano de Trazabilidad Distribuida en C

Prototipo de trazabilidad distribuida sobre **GNU/Linux** implementado en C (`-std=gnu11`) utilizando creación de procesos mediante `fork()` y `execv()`, comunicación interprocesos (IPC) con **pipes anónimos**, propagación de contexto (`trace_id`, `span_id`, `parent`) y recolección/visualización jerárquica de trazas en terminal.

---

## 1. Requisitos y Compilación

### Prerrequisitos
- Sistema operativo **GNU/Linux** (nativo o WSL2 con Ubuntu).
- Compilador `gcc >= 9` y `GNU Make`.
- Herramienta `pstree` (paquete `psmisc`) para inspección del árbol de procesos.

### Comandos del `Makefile`
```bash
# Compilar los 4 binarios en ./bin/ (launcher, collector, api, inventario)
make

# Ejecutar las pruebas unitarias de spans y E/S por pipes
make test

# Eliminar el directorio ./bin/
make clean
```

---

## 2. Ejecución

### Ejecución completa del sistema
```bash
./bin/launcher
```

### Prueba aislada del colector (Fase 2)
```bash
gcc -std=gnu11 -Wall -Wextra -O2 -Iinclude tools/gen_spans_prueba.c src/common.c -o bin/gen
./bin/gen | ./bin/collector 0
./bin/collector 0 < /dev/null
./bin/gen --huerfano | ./bin/collector 0
```

---

## 3. Arquitectura y Comunicación entre Procesos (IPC)

```text
                          +------------------+
   (peticiones) preq      |       api        |   pin    +----------------+
launcher ---------------->| span raíz        |--------->|   inventario   |
                          | genera trace_id  |<---------| span hijo      |
                          +--------+---------+   pout   +--------+-------+
                                   | psp                         | psp
                                   v                             v
                              +------------------------------------+
                              |             collector              |
                              |  lee spans, arma árbol, imprime    |
                              +------------------------------------+
```

### Mapeo con arquitecturas de observabilidad reales
- **Instrumentación (SDK OpenTelemetry):** funciones `span_ini()` y `span_fin()` en `api` e `inventario`, y propagación del contexto en `ctx_t`.
- **OpenTelemetry Collector:** proceso `collector` que recibe spans por el pipe compartido `psp`.
- **Backend de almacenamiento (Grafana Tempo):** arreglo dinámico en memoria del `collector` agrupado por `trace_id`.
- **Visualizador (Grafana):** reconstrucción recursiva e impresión del árbol indentado en terminal (`imprimir_arbol`).

### Tabla de Pipes y Descriptores por Proceso

El proceso padre (`launcher`) crea 4 pipes anónimos (8 descriptores) antes de invocar `fork()`. Cada proceso cierra inmediatamente todos los descriptores que no utiliza antes de `execv()`:

| Pipe | Sentido | Lector | Escritor | Mensaje |
|------|---------|--------|----------|---------|
| `preq` | `launcher` → `api` | `api` (`preq[0]`) | `launcher` (`preq[1]`) | `req_t` |
| `pin` | `api` → `inventario` | `inventario` (`pin[0]`) | `api` (`pin[1]`) | `ctx_t` |
| `pout` | `inventario` → `api` | `api` (`pout[0]`) | `inventario` (`pout[1]`) | `resp_t` |
| `psp` | `api`, `inventario` → `collector` | `collector` (`psp[0]`) | `api`, `inventario` (`psp[1]`) | `span_t` |

| Proceso | Descriptores que conserva | Descriptores que cierra antes de `execv` |
|---------|---------------------------|------------------------------------------|
| `collector` | `psp[0]` | `preq[0]`, `preq[1]`, `pin[0]`, `pin[1]`, `pout[0]`, `pout[1]`, `psp[1]` |
| `inventario` | `pin[0]`, `pout[1]`, `psp[1]` | `preq[0]`, `preq[1]`, `pin[1]`, `pout[0]`, `psp[0]` |
| `api` | `preq[0]`, `pin[1]`, `pout[0]`, `psp[1]` | `preq[1]`, `pin[0]`, `pout[1]`, `psp[0]` |
| `launcher` (padre) | `preq[1]` (mientras envía peticiones) | `preq[0]`, `pin[0]`, `pin[1]`, `pout[0]`, `pout[1]`, `psp[0]`, `psp[1]` |

### Cascada de cierre (EOF)
1. `launcher` envía las 6 peticiones de prueba y cierra `preq[1]`.
2. `api` recibe `EOF` (`0`) al leer `preq[0]`, cierra sus descriptores (`preq[0]`, `pin[1]`, `pout[0]`, `psp[1]`) y termina con código `0`.
3. Al cerrarse `pin[1]`, `inventario` recibe `EOF` al leer `pin[0]`, cierra sus descriptores (`pin[0]`, `pout[1]`, `psp[1]`) y termina con código `0`.
4. Al haberse cerrado todas las copias de `psp[1]` (tanto en `launcher`, como en `api` e `inventario`), `collector` recibe `EOF` en `psp[0]`, imprime el árbol de trazas, libera su memoria dinámica y termina con código `0`.
5. `launcher` recoge el estado de sus 3 hijos con `waitpid()`, evitando procesos *zombie*.

---

## 4. Decisiones de Diseño

- **Mensajes binarios de tamaño fijo:** Todas las lecturas y escrituras por pipe transfieren `sizeof(struct)` bytes mediante `leer_exacto()` y `escribir_exacto()`. Dado que `sizeof(span_t) == 264 <= 512` bytes (verificado con `assert(sizeof(span_t) <= 512)`), el kernel Linux garantiza que las escrituras concurrentes de `api` e `inventario` sobre `psp[1]` son atómicas y nunca se entremezclan.
- **Medición de tiempo monotónico:** Se utiliza `clock_gettime(CLOCK_MONOTONIC)` para evitar saltos por ajustes del reloj del sistema y permitir comparaciones consistentes entre procesos de la misma máquina.
- **Semilla aleatoria por proceso:** Cada proceso inicializa `srand(getpid() ^ time(NULL))` al generar IDs o latencias, evitando secuencias repetidas tras `fork()`/`execv()` y manteniéndose estrictamente dentro de las primitivas vistas en el curso.

---

## 5. Evidencia de Ejecución y Pruebas de Robustez (Fase 5)

### 5.1 Salida completa de `./bin/launcher`
```text
TRACE bf6488ef12030000d3587e5be5ebc237
  api        GET /products         20.35 ms  pid 866  OK
     `- inventario consultar_catalogo    16.07 ms  pid 865  OK
TRACE 471d214b8e770af2fffa0503fa05d652
  api        GET /health            9.11 ms  pid 866  OK
TRACE 777f66c14042085d6454ecdb5ededa58
  api        POST /buy             31.45 ms  pid 866  OK
     `- inventario reservar_stock        22.19 ms  pid 865  OK
TRACE dbda4be18e9f590e051a4e4722acab76
  api        POST /buy            340.54 ms  pid 866  OK
     `- inventario reservar_stock       336.15 ms  pid 865  OK  <-- anomalia
TRACE 43f226afd8038ab34e6c41edc54ff2df
  api        POST /buy             32.50 ms  pid 866  ERROR
     `- inventario reservar_stock        27.10 ms  pid 865  ERROR  <-- anomalia
TRACE 58cd7ab20bfbf5fe21a5d6242f89729b
  api        POST /buy             36.37 ms  pid 866  ERROR
     `- inventario reservar_stock        27.10 ms  pid 865  ERROR
-- 6 trazas, 11 spans
[launcher] collector terminó con código 0
[launcher] inventario terminó con código 0
[launcher] api terminó con código 0
```

### 5.2 Jerarquía de procesos (`ps -ef --forest` y `pstree -p`) y ausencia de zombies
Durante la ejecución:
```text
$ ps -ef --forest | grep -E 'bin/(launcher|api|inventario|collector)'
xavih830     817     546  0 22:35 pts/0    00:00:00  |           \_ ./bin/launcher
xavih830     819     817  0 22:35 pts/0    00:00:00  |           |   \_ ./bin/collector 9
xavih830     820     817  0 22:35 pts/0    00:00:00  |           |   \_ ./bin/inventario 5 8 10
xavih830     821     817  0 22:35 pts/0    00:00:00  |           |   \_ ./bin/api 3 6 7 10

$ pstree -p $(pgrep launcher)
launcher(828)-+-api(832)
              |-collector(830)
              `-inventario(831)
```
Tras finalizar `./bin/launcher`, `ps -ef | grep defunct` confirma **0 procesos zombie (`<defunct>`)**.

### 5.3 Comportamiento ante caída abrupta de un hijo (`kill -9 inventario`)
Al terminar abruptamente `inventario` con `kill -9` en mitad de la ejecución:
1. El kernel cierra inmediatamente los descriptores de `inventario` (`pin[0]`, `pout[1]`, `psp[1]`).
2. `api` detecta `EOF` inesperado en `pout[0]`, marca su span actual en estado `ERROR` (`inv: eof_inesperado`), lo envía al colector, cierra todos sus descriptores y termina con código `1`.
3. `collector` recibe `EOF` en `psp[0]`, imprime las trazas recolectadas hasta el incidente y termina con código `0`.
4. `launcher` recoge los estados con `waitpid` y reporta la señal y el error sin colgarse:
```text
TRACE 3723513410420619996350fa35d9f26b
  api        GET /products         42.39 ms  pid 838  OK
     `- inventario consultar_catalogo    35.10 ms  pid 837  OK
TRACE 713602a3f2b1db15030f2545153ede78
  api        GET /health            7.19 ms  pid 838  OK
TRACE b8b65dca19083b4f0ade41bcb956bfc8
  api        POST /buy             42.30 ms  pid 838  OK
     `- inventario reservar_stock        35.07 ms  pid 837  OK
TRACE b1861d664ebf1f041de91e25246d3002
  api        POST /buy             59.06 ms  pid 838  ERROR
-- 4 trazas, 6 spans
[launcher] collector terminó con código 0
[launcher] inventario terminó por señal 9
[launcher] api terminó con código 1
```

### 5.4 Tiempo de ejecución (`time ./bin/launcher`)
```text
real	0m0.469s
user	0m0.004s
sys	0m0.000s
```

---

## 6. Matriz de Trazabilidad con el Enunciado

| Requisito del enunciado | Dónde se cumple | Evidencia |
|-------------------------|-----------------|-----------|
| GNU/Linux | Entorno WSL2 Linux (`gcc 13.3.0`, `GNU Make 4.3`) | `uname -s` → `Linux` |
| Procesos con `fork`/`exec` | `src/launcher.c` (`lanzar()`) | `pstree -p` y `ps -ef --forest` (Sección 5.2) |
| IPC con pipes anónimos | 4 pipes (`preq`, `pin`, `pout`, `psp`) | Salida completa de `./bin/launcher` |
| Trace ID / Span ID / Parent Span ID | `include/span.h`, `include/protocolo.h`, `src/common.c` | `make test` y reconstrucción del árbol |
| Medición de tiempos | `ahora_ns()`, `span_ini()`, `span_fin()` con `CLOCK_MONOTONIC` | Duraciones en ms coherentes en cada span |
| Colector centralizado | `src/collector.c` | 6 trazas, 11 spans agrupados e impresos |
| Visualización en terminal | `imprimir_arbol()` en `src/collector.c` | Sección 5.1 |
| Manejo de errores y cierre limpio | `leer_exacto()`, `escribir_exacto()`, cascada de EOF, `waitpid()` | Sección 5.2 y 5.3 (prueba `kill -9`) |
| `Makefile` y `README.md` | Raíz del proyecto | Compilación limpia con `-Wall -Wextra` |
