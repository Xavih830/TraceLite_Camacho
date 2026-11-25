# PLAN — TraceLite, entrega final con eBPF

- Fecha: 2026-11-24.
- Ejecutor: agente Gemini 4 Hydrogen (medium).
- Revisión 2026-11-24: el punto de vista **principal** son los servicios ya en ejecución; se agregan modos por objetivo con plazos justificados.
- Revisión 2026-11-24 (2): las solicitudes se integran en **instantánea** y **resumen** (sin subcomando `solicitudes`); niveles de tiempo; plan de interfaz gráfica.
- Entorno objetivo: Lubuntu (Ubuntu 24.04, kernel 7.0.0-43-generic, gcc 13.3, 16 núcleos; según `docs/PLAN_PRUEBAS_LUBUNTU.md`).

## 0. Reglas para el agente ejecutor

1. Lee las secciones 0 a 5 antes de escribir código.
2. Ejecuta **una sola fase** por turno. Corre sus pruebas de aceptación y detente. No adelantes fases.
3. Si una prueba falla, corrígela o reporta la salida real. No marques la fase como terminada.
4. No cambies los contratos de la sección 4 sin preguntar.
5. No inventes resultados. Toda duración, PID o ID de la documentación sale de una ejecución real.
6. Al terminar cada fase: marca su casilla en `CHECKLIST.md` y **agrega** una entrada al final de `docs/Updates.md`. No edites entradas anteriores.
7. Compila y prueba en Linux, dentro de un sistema de archivos ext4. No uses `/mnt/c` ni NTFS: `setcap` no funciona allí.
8. No ejecutes `sudo` por tu cuenta. Muestra el comando al usuario y espera.
9. No hagas commit ni push sin pedido del usuario.
10. No empieces la interfaz gráfica (fases G1 y G2, sección 8) sin visto bueno explícito del usuario (D5).
11. Estilo: C `gnu11`, sin advertencias con `-Wall -Wextra`, nombres en español, como el código actual.
12. Nunca envíes señales ni cierres un servicio que TraceLite no inició (sección 4.8). No inventes fuentes ni valores de plazo: usa solo la sección 10 y marca como «decisión propia» lo que no tenga fuente.
13. **Comentarios.** Conserva los existentes sin reescribirlos, moverlos ni borrarlos; cambia uno solo si dejó de ser cierto, y entonces edita lo mínimo. Los nuevos siguen su forma:
    - `/* ... */` en español, de una línea; sin `//`, bloques decorativos ni Doxygen.
    - Uno al inicio de cada módulo con su propósito. Uno sobre una función solo si su propósito no es evidente por el nombre. Pasos numerados (`/* 1. ... */`) solo en las secuencias principales.
    - Resaltan el punto principal o un porqué no obvio. No describen línea por línea ni citan fases, este plan o el historial de cambios.
    - Los fragmentos C de este plan se adaptan a esta regla; sus comentarios no se copian tal cual si sobran.

`docs/PLAN_MVP_TRACELITE.md` es un antecedente: sus pipes y su lista negra no rigen. El avance 1 entregado usa Sockets UNIX y `pthread`; **ese es el IPC que se conserva**.

## 1. Qué se debe lograr

### 1.1 Requisitos del enunciado

| ID | Requisito | Fase |
|---|---|---|
| R1 | Dos procesos hijos, Servicio A y Servicio B, con `fork()`; `exec()` en al menos uno; IPC del avance 1 | F2 |
| R2 | Al menos 5 solicitudes, 2 spans cada una (A raíz, B hijo), campos completos, contexto propagado, duración medida | F2 |
| R3 | El proceso principal es Collector Lite: recibe por IPC y muestra por Trace ID con padre-hijo | F2 |
| R4 | Persistencia justificada y demostrable; resumen final de solicitudes, spans y alertas | F3 |
| R5 | Anomalía detectada con criterio justificado; alerta con servicio y Trace ID | F4 |
| P1 | Prueba: 4 o más solicitudes normales, sin alerta, spans almacenados | F4 |
| P2 | Prueba: 1 o más solicitudes anómalas, con detección, alerta y persistencia | F4 |
| P3 | Prueba: ambos spans comparten Trace ID y `B.parent == A.span_id` | F2, F4 |
| E1 | Informe, código, Makefile, README, checklist y video de 10 min | F10 (video: usuario) |

### 1.2 Objetivos del usuario

| ID | Objetivo | Cómo se cubre | Fase |
|---|---|---|---|
| U1 | Usar programas reales y cotidianos, no servicios simulados | A entrega la solicitud a un programa real; B observa su efecto | F2 |
| U2 | Funcionar con cualquier programa que el usuario pueda ejecutar | Comando arbitrario después de `--` | F2 |
| U3 | Saber qué procesos corresponden a cada solicitud | eBPF: marca heredada en cada `fork` del kernel | F5 |
| U4 | Observar servicios que ya están corriendo (punto de vista **principal**) | Destinos `backend` y `app`; el servicio sigue vivo después de las solicitudes | F6–F8 |
| U5 | El SO empuja los datos a un punto de memoria propio | `BPF_MAP_TYPE_RINGBUF` (memoria del kernel) | F5 |
| U6 | Conservar solo los últimos 7 días | Índice en memoria y segmentos diarios con retención | F3 |
| U7 | Interfaz gráfica en C | Programa separado `tracelite-gui` (sección 8) | G1, G2 |
| U8 | Modos según el objetivo, con 5 solicitudes por modo y plazo justificado con fuentes | Sección 4.8 y sección 10 | F9 |
| U9 | Cada modo vale para servicios nuevos y para servicios ya vivos | Matriz de la sección 4.8 | F6–F9 |
| U10 | Reutilizar instantánea y resumen como base, sin empezar de cero | Secciones 2.2 y 3 | F2–F9 |
| U11 | Niveles de tiempo según la complejidad de la prueba | `--tiempo` (4.10) | F9 |

### 1.3 Pautas del curso

Alcance declarado: introducción, estructuras del SO, procesos e IPC, hilos. Cada pieza debe poder explicarse desde ahí.

| Pieza | Tema del curso |
|---|---|
| `fork`, `execvp`, `waitpid`, barrera por `pipe` | Procesos |
| Sockets UNIX | IPC, paso de mensajes |
| `pthread` y mutex | Hilos |
| eBPF | Estructuras del SO: depuración y trazado del kernel (Silberschatz 10.ª ed. trata BCC en el capítulo 2) |
| Ring buffer y mapas eBPF | Memoria compartida kernel–usuario, productor–consumidor |

eBPF es una ampliación: el usuario debe **confirmarla con el profesor**. Por eso F0–F4 cumplen todo el enunciado sin eBPF. Si eBPF se rechaza o no carga, la entrega sigue completa.

## 2. Idea central

Una **solicitud** es una acción concreta pedida a un programa real. Genera una traza con dos spans:

- **Servicio A** (`entrega`, raíz): recibe la solicitud, crea el contexto, entrega la acción al programa y cubre la solicitud completa.
- **Servicio B** (`observa`, hijo): recibe el contexto de A y mide qué hizo el SO para atender la solicitud.

A y B no simulan trabajo: A entrega y B mide el trabajo real. El programa observado no se modifica.

```text
                    tracelite  (proceso principal = Collector Lite)
                    | acepta spans, detecta, alerta, persiste, muestra
        fork+execv  |                                    fork+execv
          +---------+----------------------------------------+
          v                                                  v
   servicio_a (raíz) ---- AF_UNIX: ctx_t / resp_t ----> servicio_b (hijo)
          |                                                  ^
          | fork + execvp                                    | ring buffer + mapas
          v                                                  |
   programa real ---- fork/exec/exit/CPU ----> kernel: programas eBPF
                                               (o /proc como respaldo)

   servicio_a y servicio_b ---- AF_UNIX: span_t ----> tracelite ----> JSONL diario
```

A y B son hijos directos de `tracelite`. El programa real es hijo de A.

### 2.1 Dos puntos de vista

TraceLite se aplica a dos situaciones. Ambas usan los mismos A, B, colector y almacén.

| Punto de vista | Qué es | Ejemplo | Prioridad |
|---|---|---|---|
| **Servicio vivo** | El servicio ya corre antes de la solicitud 1 y sigue vivo después de la 5. Es lo que ocurre con un backend real: no se apaga después de cada solicitud | `python3 backend_demo.py` ya abierto; Firefox ya abierto | **Principal** |
| **Servicio nuevo** | TraceLite lo inicia. Si es un servicio, lo inicia **una vez**, antes de la solicitud 1, y lo mantiene vivo hasta la 5. Si es un comando, crea un proceso por solicitud | `--lanzar-backend`, `-- gzip …` | Base del enunciado |

Quién inició el servicio **no** cambia cómo entra la solicitud. Un servicio que ya está vivo recibe la solicitud por su canal (un socket), no por herencia de proceso. Por eso la atribución depende del **destino** y no de quién lo inició (secciones 4.8 y 5.4). Un comando de un solo uso (`gzip`) solo existe como «nuevo».

### 2.2 Base existente: instantánea y resumen

No se empieza de cero. Las solicitudes se activan **dentro** de los dos modos actuales. Cada ejecución combina tres ejes:

| Eje | Valores |
|---|---|
| Forma | **Instantánea** (por defecto): 1 solicitud. **Resumen** (`--resumen`): serie de 5 solicitudes |
| Destino | `programa`, `backend`, `app` (4.8). Sin destino de solicitud: observación, como hoy |
| Modo | `respuesta`, `estabilidad`, `escalado`, `fallo` (4.8) |

Estado del código actual, leído en `src/`:

| Pieza | Ya existe | Falta |
|---|---|---|
| `fork` + `execv` | Sí, para `collector` y `monitor_pid` | Procesos con papel de A y B |
| Contexto | **Sí**: la instantánea pasa `trace_id`, `span_id` y `parent` a `monitor_pid` por `argv`. En resumen no cruza procesos: `tracelite` crea todos los spans | Que una **solicitud** lo transporte de A a B por IPC |
| Solicitud | No: una ejecución es una traza de N procesos | Una acción con inicio y fin propios, y 2 spans |
| Duración | Instantánea: espera fija de 250 ms. Resumen: tiempo en que se vio el proceso | Duración de la solicitud |
| Colector | Es un proceso hijo | R3 lo pide en el proceso principal |
| Anomalía | Compara procesos de una misma traza | Comparar solicitudes de la misma operación |
| Persistencia y alerta por Trace ID | No | 4.5 y 4.6 |

Se reutilizan: lectura de `/proc` y del árbol, `buscar_pid_por_nombre`, `fork` + `execv`, Sockets UNIX, `span_ini`/`span_fin`, E/S exacta, hilos del colector, tabla y el criterio base de anomalía.

**Observación.** `tracelite [PID|NOMBRE]` y `tracelite --resumen [PID|NOMBRE]`, sin destino de solicitud, conservan su salida. Se muestran como «observación»: no son solicitudes, no cuentan en `solicitudes=` y no son evidencia de R2, porque no hay una acción con inicio y fin. En F3 sus spans también se guardan, con `tipo` = `observacion`.

## 3. Comandos objetivo

```bash
# Observación (sin solicitudes; como hoy)
./bin/tracelite [PID|NOMBRE]
./bin/tracelite --resumen -d 30 --interval 150 [PID|NOMBRE]

# INSTANTÁNEA (por defecto): 1 solicitud
./bin/tracelite -- gzip -kfc datos/normal.bin
./bin/tracelite --modo respuesta --backend 127.0.0.1:8000 --ruta "/hash?mb=8"

# RESUMEN: 5 solicitudes según el modo
# Destino programa (nuevo: un proceso por solicitud)
./bin/tracelite --resumen --modo estabilidad -- gzip -kfc datos/normal.bin
./bin/tracelite --resumen --lote docs/pruebas/lote_anomalo.txt

# Destino backend VIVO (ya corre en 127.0.0.1:8000)
./bin/tracelite --resumen --modo respuesta --backend 127.0.0.1:8000 --ruta "/hash?mb=8"

# Destino backend NUEVO (TraceLite lo inicia una vez y lo detiene al final)
./bin/tracelite --resumen --modo respuesta --lanzar-backend "python3 docs/pruebas/backend_demo.py 8000" --ruta "/hash?mb=8"

# Destino app VIVA (ya abierta). La orden va después de --
./bin/tracelite --resumen --modo respuesta --app firefox -- firefox --new-tab https://example.com

# Destino app NUEVA (TraceLite la inicia una vez; verifica en el equipo que la orden llegue a esa instancia)
./bin/tracelite --resumen --modo respuesta --lanzar-app "firefox --profile /tmp/perfil_tl" -- firefox --profile /tmp/perfil_tl --new-tab https://example.com

# Prueba más compleja: más tiempo límite
./bin/tracelite --resumen --modo escalado --tiempo alto --backend 127.0.0.1:8000 --ruta /hash --entradas "mb=32,mb=64,mb=128,mb=256,mb=512"

# Historial
./bin/tracelite --historial
./bin/tracelite --historial --trace <trace_id>
```

Opciones comunes:

- `--modo respuesta|estabilidad|escalado|fallo`: por defecto `respuesta` en instantánea y `estabilidad` en resumen (4.8).
- `-n N`: número de solicitudes; 5 por defecto. Solo con `--resumen`.
- `--tiempo normal|alto|muy-alto`: límite de tiempo según la complejidad (4.10); `normal` por defecto.
- `--plazo SEG`: límite exacto, para pruebas. Es incompatible con `--tiempo`.
- `--entradas a,b,c,d,e`: variantes de la acción, una por solicitud. En `backend`, cada una es la consulta que se añade a `--ruta` tras `?`; en `programa` y `app`, sustituye el último argumento de la orden.
- `--tamanos 1,2,4,8,16`: peso de cada entrada (modo `escalado`).
- `--slo-ms MS`, `--fallo-en N`, `--linea-base SEG`: ver 4.8.
- `--fuente auto|ebpf|proc`: `auto` usa eBPF si carga; si no, avisa en una línea y usa `/proc`. `ebpf` termina con error si no carga.
- `--lote ARCHIVO`: una solicitud por línea; `#` comenta. Solo con `--resumen` y sin orden tras `--`.
- `--mostrar-salida`: muestra la salida estándar del programa; por defecto va a `/dev/null`.

Reglas:

- El lote se separa por espacios simples. No hay comillas, variables ni tuberías. No uses `system()`.
- No hay subcomandos. Sin destino de solicitud, se conserva el comportamiento actual.
- Solo un destino por ejecución. `-- orden` sin `--app`/`--lanzar-app` es `programa`; con ellos, es la orden de la app.
- `-d` e `--interval` valen solo en observación. Las opciones de solicitud no se combinan con un PID o nombre suelto. Toda combinación inválida termina con código 2 y un mensaje claro; hoy el parser ignora las opciones desconocidas.
- La instantánea no tiene otras solicitudes para comparar. Admite `respuesta` (`slo_excedido`) y `estabilidad` (`latencia_alta` solo con 3 o más referencias en el historial), además de `error_proceso`. `escalado` y `fallo` exigen `--resumen`.
- `estabilidad` en instantánea no calibra: usa la cota de 4.10.
- Las solicitudes son **secuenciales**: solo una activa a la vez. Así se evita la ambigüedad entre solicitudes.

## 4. Contratos

### 4.1 `span_t`

Conserva los campos actuales en el mismo orden y agrega al final:

```c
double cpu_ms;          /* CPU atribuida; 0 en A */
int    n_procs;         /* procesos atribuidos; 0 en A */
char   atribucion[12];  /* "exacta", "conexion", "ventana" o "" */
```

El tamaño esperado pasa de 264 a 288 bytes. Verifícalo en `make test`. Todos los binarios deben recompilarse juntos.

Valores: `servicio` es `servicio_a` o `servicio_b`; `operacion` es el nombre base del programa **sin argumentos** (`gzip`); `pid` es el del servicio en A y el del programa observado en B. La línea completa se registra en `msg` de A, truncada.

### 4.2 Mensajes A–B (`include/protocolo.h`)

```c
typedef enum { MSG_INICIAR = 1, MSG_LISTO, MSG_TERMINAR, MSG_RESULTADO, MSG_FIN, MSG_ERROR } msg_tipo_t;

typedef struct {            /* A -> B */
    int  tipo;              /* INICIAR, TERMINAR o FIN */
    int  solicitud;
    char trace_id[33];
    char parent_span[17];   /* span_id de A */
    int  pid_programa;
    int  pid_app;           /* 0 si se lanza un programa nuevo */
    int  estado_salida;     /* válido en TERMINAR: 0, código de salida o 128+señal */
    char operacion[48];
} ctx_t;

typedef struct {            /* B -> A */
    int     tipo;           /* LISTO, RESULTADO o ERROR */
    int     solicitud;
    int64_t fin_ns;         /* última actividad, CLOCK_MONOTONIC */
    int     estado;
    char    msg[64];
} resp_t;
```

`sol_t` (tracelite → A) lleva tipo, número de solicitud y línea del comando (512 bytes). `sol_resp_t` (A → tracelite) lleva tipo, número y estado. Usa `escribir_exacto` y `leer_exacto`.

### 4.3 Canales

- Directorio privado `0700` por ejecución, creado con `mkdtemp`: `$XDG_RUNTIME_DIR/tracelite-XXXXXX`; si falta la variable, `/tmp/tracelite-XXXXXX`. Vale para observación y solicitudes.
- Comprueba el truncamiento de toda ruta antes de copiarla a `sun_path` (108 bytes); si no cabe, error claro.
- `tracelite` es el dueño: borra los sockets y el directorio en su flujo normal de cierre, después de `waitpid`. El manejador de señal solo marca una bandera `volatile sig_atomic_t`. Si ocurre `SIGKILL`, el directorio puede quedar; `mkdtemp` evita que estorbe a la siguiente ejecución.
- Dentro: `spans.sock`, `a.sock` y `b.sock`, todos `AF_UNIX SOCK_STREAM`. Pasa las rutas por `argv`.
- Todo `recv` de control tiene plazo (`SO_RCVTIMEO`). `SIGPIPE` se ignora en los servicios, pero se **restaura a `SIG_DFL`** en el hijo antes de `execvp`.
- Todo descriptor propio usa `CLOEXEC` para que el programa real no lo herede.
- Al terminar o con `Ctrl+C`: cierre ordenado, `waitpid` de A y B, borrado de sockets y directorio.

### 4.4 Secuencia de una solicitud (destino `programa`)

1. `tracelite` envía `sol_t` a A.
2. A ejecuta `span_ini` (raíz) y crea un `pipe` de barrera.
3. A hace `fork()`. El hijo **espera** en la barrera.
4. A envía `MSG_INICIAR` a B con `trace_id`, su `span_id` y el PID del hijo.
5. B marca el PID, abre su span con ese contexto y responde `MSG_LISTO`.
6. A libera la barrera. El hijo hace `execvp`. Así no se pierde ningún evento inicial.
7. El programa trabaja. A espera con `waitpid` y un plazo (4.10; hasta F9, `--plazo` o 60 s). Al vencer: `SIGTERM`, 2 s, `SIGKILL`; estado ERROR `plazo_vencido`. Señala al **grupo del programa** (`setpgid` en el hijo y en A). Esto vale solo para programas que TraceLite lanzó para esa solicitud; para servicios, ver 4.8.
8. A envía `MSG_TERMINAR` con el estado de salida.
9. B espera a que no queden procesos marcados, con un máximo de 2 s; si quedan, lo anota en `msg`. Cierra su span, lo envía a `tracelite` y responde `MSG_RESULTADO`.
10. A cierra su span con `fin = max(fin del programa, resp.fin_ns)` y lo envía. La espera de confirmación no infla la duración.
11. A responde a `tracelite`. El colector considera completa la solicitud cuando tiene **ambos spans**, no antes.

Invariantes: `A.ini <= B.ini`, `B.fin <= A.fin`, mismo `trace_id`, `B.parent == A.span_id`, duraciones no negativas. `CLOCK_MONOTONIC` y `bpf_ktime_get_ns()` usan la misma base.

Si `execvp` falla, el hijo termina con 127 y ambos spans quedan ERROR. Si B muere, A cierra su span con ERROR `servicio_b_caido`; `tracelite` lo informa y no se bloquea.

### 4.5 Registro persistente

- Directorio: `$TRACELITE_DIR`; si falta, `$HOME/.local/share/tracelite`. Las pruebas **siempre** usan un directorio temporal.
- Un archivo por día local: `AAAA-MM-DD.jsonl`, abierto en modo append. Un span por línea.
- Claves: `v` (=1), `tipo` (`solicitud` u `observacion`), `modo`, `destino`, `tiempo`, `plazo_s`, `solicitud`, `trace_id`, `span_id`, `parent`, `servicio`, `operacion`, `ini_ns`, `fin_ns`, `ini_real_ns`, `fin_real_ns`, `dur_ms`, `estado`, `msg`, `anomalia`, `pid`, `cpu_ms`, `n_procs`, `atribucion`.
- `*_real_ns` es hora de pared, calculada por el colector al recibir. Los tiempos monotónicos no sirven entre reinicios.
- El número `solicitud` lo asigna `tracelite`, que correlaciona cada span por su `trace_id` (incluye `trace_id` en `sol_resp_t`).
- Escribe las dos líneas cuando la solicitud esté completa y ya evaluada, seguidas de `fflush` y `fsync`. Si falta un span al terminar, escribe el que llegó con `msg` `traza_incompleta`.
- Escapa en JSON comillas, barra invertida y caracteres de control; los bytes no UTF-8 se sustituyen por `?`.
- El lector ignora y cuenta líneas corruptas. Nunca aborta por una línea truncada.

**Retención.** `--retencion-dias` (7 por defecto) y `--retencion-seg` (solo demostración). Al iniciar y tras cada solicitud: el índice en memoria descarta spans con `fin_real_ns` vencido, y se borra un segmento solo cuando **todos** sus spans vencieron. Límite: 100 000 spans en memoria; al llegar, sale el más antiguo. Borra únicamente archivos del directorio de datos que cumplan exactamente `AAAA-MM-DD.jsonl`.

**Justificación para el informe.** El JSONL append-only por segmentos sobrevive al proceso y al reinicio, se inspecciona con herramientas estándar, pierde como mucho la última línea ante un corte y permite eliminar bloques completos por antigüedad. Se inspira en los bloques de Tempo; no usa su formato.

### 4.6 Anomalía y alerta

Por cada solicitud completa, evalúa el span de **B**. La referencia son hasta los 50 spans previos más recientes con igual `servicio`, `operacion` y `atribucion`, estado OK y sin anomalía, incluyendo el historial vigente.

Se marca `latencia_alta` cuando hay al menos 3 referencias y se cumplen a la vez:

- `d > mu + 2*sigma`
- `d > 1.5*mu`
- `d - mu >= 10 ms`

El span actual no participa en su referencia y los anómalos tampoco. Las dos últimas condiciones evitan alertas por variación pequeña. A no se evalúa por latencia, pues contiene a B. El error se marca `error_proceso` y alerta sobre B; si B no existe, sobre A. Hay **una alerta por solicitud**.

```text
[ALERTA] servicio=servicio_b trace_id=<32 hex> tipo=latencia_alta operacion=gzip dur=812.40ms mu=101.22ms umbral=118.90ms
```

La alerta sale al completarse la solicitud, en rojo solo si stdout es terminal. No modifiques la regla Leave-One-Out del observador existente.

Resumen final exacto:

```text
-- Resumen: solicitudes=5 spans=10 alertas=1 errores=0 --
```

### 4.7 Fuente de observación (`include/fuente.h`)

B usa una interfaz única con dos implementaciones, `fuente_proc.c` y `fuente_ebpf.c`:

```c
typedef struct {
    int     n_procs;
    int     n_vivos;
    double  cpu_ms;
    int64_t ultima_actividad_ns;
    char    atribucion[12];
    unsigned long perdidos;   /* eventos descartados */
} medida_t;

typedef struct {
    int  (*abrir)(void);
    int  (*iniciar)(int pid_programa, int pid_app, int solicitud);
    int  (*leer)(medida_t *m);
    void (*detener)(void);
    void (*cerrar)(void);
} fuente_t;
```

`fuente_proc` muestrea cada 20 ms en un hilo de B y sigue el PPID; identifica procesos por **PID más `starttime`** (campo 22 de `stat`) para no confundir PIDs reciclados. Puede perder procesos cortos, y su `atribucion` es `ventana`. `fuente_ebpf` da `exacta` al lanzar.

### 4.8 Destinos, modos y plazos

**Destinos.** Cada uno define qué es una solicitud y cuándo termina.

| Destino | Opciones | Una solicitud es | Termina cuando | Atribución |
|---|---|---|---|---|
| `programa` | `-- cmd` | Un proceso nuevo por solicitud | El proceso termina | `exacta` (eBPF) o `ventana` (`/proc`) |
| `backend` | `--backend H:P` (vivo), `--lanzar-backend "cmd"` (nuevo) | Una petición HTTP/1.0 por TCP a `127.0.0.1` que envía A | A recibe el fin de la respuesta | `conexion` (eBPF) o `ventana` (`/proc`) |
| `app` | `--app NOMBRE\|PID` (viva), `--lanzar-app "cmd"` (nueva), más `-- orden` | Una orden que A ejecuta contra la app | La app queda inactiva | `ventana` |

Un comando de un solo uso solo existe como `programa` nuevo. Los servicios (`backend`, `app`) admiten ambos puntos de vista.

**Servicios nuevos.** TraceLite los inicia una sola vez, antes de la solicitud 1, espera hasta 5 s a que acepten conexiones y los detiene al final (`SIGTERM`, 2 s, `SIGKILL`). Las cinco solicitudes usan el **mismo** proceso.

**Servicios vivos.** TraceLite **nunca** los cierra ni les envía señales. Si vence el plazo, solo deja de medir y escribe `plazo_vencido` en `msg`. Al terminar, el servicio conserva el mismo PID.

**Petición HTTP de A (destino `backend`).**

```text
GET <ruta> HTTP/1.0\r\nHost: <host>\r\ntraceparent: 00-<trace_id>-<span_id_de_A>-01\r\nConnection: close\r\n\r\n
```

El encabezado `traceparent` sigue el formato W3C Trace Context [S7]: versión, `trace_id` (32 hex), `parent_id` (16 hex) y banderas. Un backend instrumentado continúa la traza; uno que no lo está lo ignora. A descarta el cuerpo y registra el número de bytes en `msg`. `estado` de A es ERROR si no hay conexión, vence el plazo o el código HTTP es ≥ 400 (decisión propia: A es el cliente). `operacion` es `GET <ruta>` sin la consulta.

**Orden de app (destino `app`).** A la ejecuta con `fork` + `execvp` y plazo normal. Su código de salida 0 es OK. La medición de B **no** depende de que la orden termine, porque la orden suele regresar antes de que la app termine el trabajo.

**Línea base (servicios vivos y apps).** Antes de la solicitud 1, B mide `--linea-base` segundos (5 por defecto, decisión propia) la CPU y los procesos del servicio sin solicitudes. Al cerrar cada solicitud, `cpu_ms` es la CPU medida menos `tasa_base × duración` (mínimo 0), y `msg` registra `fondo=<ms>`. Si la línea base supera 25 % de un núcleo (decisión propia), TraceLite avisa que la atribución será pobre.

**Inactividad (destino `app`).** B divide el tiempo en ventanas de 100 ms. La app está inactiva tras 3 ventanas seguidas con CPU menor o igual a `tasa_base × 100 ms + 2 ms` (decisión propia; calibra en el equipo). `fin` es el último instante con actividad. Si llega el plazo sin reposo, escribe `sin_reposo` en `msg`; no es error.

**Modos.** En resumen, cada modo genera 5 solicitudes. Fija plazo, regla de fin y criterio de alerta. Valen para todos los destinos, nuevos y vivos.

| Modo | Qué se verifica | Las 5 solicitudes | Plazo por solicitud (`--tiempo normal`) | Alerta |
|---|---|---|---|---|
| `respuesta` | ¿La acción se percibe rápida? | 5 acciones, iguales o `--entradas` | 10 s [S1] | `slo_excedido` si `dur > --slo-ms`. Por defecto: 200 ms en `app` [S2] y 1000 ms en `programa` y `backend` [S1] (decisión propia sobre qué umbral va en cada destino). Se registra la clase: ≤ 100 ms instantánea, ≤ 1 s fluida, ≤ 10 s atención, > 10 s excedida [S1] |
| `estabilidad` | ¿Tarda lo mismo cada vez? | 5 idénticas. Con `--entradas`, sirve para el caso anómalo de prueba (4 normales y 1 pesada) | `max(10 s, 3 × la mediana de una solicitud de calibración)`, tope 60 s (decisión propia). La calibración no se guarda ni cuenta | `latencia_alta` (4.6) |
| `escalado` | ¿Cómo crece el costo con la carga? | 5 entradas crecientes con pesos `--tamanos` (1, 2, 4, 8, 16 por defecto) | 30 s (decisión propia; si la quinta lo excede, reduce los tamaños o usa `--tiempo`) | `escalado_no_lineal` si `dur/peso` supera 1,5 × la mediana de `dur/peso` de las otras y la diferencia es de al menos 10 ms |
| `fallo` | ¿Se detecta y registra el error? | 4 válidas y 1 inválida (`--fallo-en N`, 5 por defecto): ruta inexistente en `backend`, archivo inexistente en `programa`, orden inválida en `app` | 10 s [S1] | `error_proceso` sobre B (sobre A si B no existe) |

Reglas de los modos:

- **Plazo.** El plazo de 10 s sale del límite de atención de [S1]. La idea de cortar al vencer el plazo, para no gastar trabajo que nadie espera, sale de [S4]. Corta solo a programas lanzados por TraceLite; a un servicio vivo no lo corta. Los demás valores son decisiones propias y así deben aparecer en el informe.
- **Cierre del lote.** `tracelite` espera hasta 5 s los spans pendientes antes de marcar `traza_incompleta` (referencia: el retraso de lote de 5 s del SDK de OpenTelemetry [S3]).
- **Evaluación.** `respuesta` y `fallo` alertan al completarse cada solicitud. `estabilidad` y `escalado` evalúan al cerrar el lote: cada solicitud contra las otras 4 y el historial vigente.
- **Referencia.** La regla de 4.6 agrega `modo` y `destino` como criterios de igualdad. El JSONL guarda `modo` y `destino` (pertenecen a `tracelite`, no a `span_t`). Solo los spans con `tipo` = `solicitud` sirven de referencia. La línea de alerta agrega `modo=<modo>` al final.
- **Muestra pequeña.** Con 5 solicitudes no se pueden calcular percentiles p95 o p99 [S5]. Se usan mediana y máximo, y el informe lo declara como limitación.

**Cobertura de la matriz.**

| Modo | `programa` nuevo | `backend` nuevo | `backend` vivo | `app` nueva | `app` viva |
|---|---|---|---|---|---|
| `respuesta` | Prueba | Prueba | Prueba | Demostración | Demostración |
| `estabilidad` | Prueba | Prueba | Prueba | Demostración | Demostración |
| `escalado` | Prueba | Prueba | Prueba | Demostración | Demostración |
| `fallo` | Prueba | Prueba | Prueba | Demostración | Demostración |

«Prueba» es automática y determinista. «Demostración» depende de una app real y no es criterio de aprobación.

### 4.9 Servicios existentes del SO

**Supuesto sin verificar:** Lubuntu tiene estos servicios. El plan se escribió desde Windows. F0 los detecta y el usuario confirma o recalibra.

| Servicio | Unidad | Canal | Solicitud | Destino y nivel esperado |
|---|---|---|---|---|
| CUPS | `cups.service` | TCP `127.0.0.1:631`, HTTP | `GET /` | `backend`: `conexion` con eBPF |
| DNS local | `systemd-resolved.service` | D-Bus, Varlink o DNS local | `resolvectl query localhost` (no depende de Internet) | `app`: `ventana` |
| snapd | `snapd.service` | Socket Unix `/run/snapd.socket`, HTTP | `curl --unix-socket /run/snapd.socket http://localhost/v2/system-info` | `app`: `ventana` |
| hostnamed | `systemd-hostnamed.service` | D-Bus | `hostnamectl` | `app`: `ventana`; puede iniciarse bajo demanda |
| Servidor de Python | ninguna (lo abre el usuario) | TCP `127.0.0.1:8000`, HTTP | `GET /archivo` | `backend`: `conexion` con eBPF |

Comandos:

```bash
./bin/tracelite --resumen --modo respuesta --backend 127.0.0.1:631 --unidad cups.service --ruta /
./bin/tracelite --resumen --modo respuesta --app systemd-resolved --unidad systemd-resolved.service --fin orden -- resolvectl query localhost
```

Reglas:

- **Solo consultas de lectura.** Nunca detengas, reinicies, configures ni habilites servicios del SO. No imprimas, no instales snaps y no resuelvas nombres ajenos al ejemplo.
- **Papel.** Son la demostración de servicios vivos. Las pruebas obligatorias usan programas y `backend_demo.py`, cuyo costo y errores se controlan.
- **PID.** `--unidad U` lo obtiene con `systemctl show -p MainPID --value U`, mediante `fork` + `execvp`. Hace falta porque `/proc/<pid>/fd` de un servicio de otro usuario no se puede leer. Si el PID es 0, el servicio está inactivo: en `backend` se informa y en `app` se rechaza.
- **Activación por socket.** CUPS y hostnamed pueden arrancar con la primera petición o cerrarse solos. Se hace una petición de calentamiento, no registrada, antes de leer el PID. Si el PID cambia durante el lote, se anota `pid_cambio`; no se afirma que el servicio siguió vivo.
- **Fin.** `--fin orden` termina cuando la orden responde; sirve para clientes síncronos (`curl`, `resolvectl`). `--fin inactividad` es el valor por defecto de `app` y sirve para órdenes que regresan antes (navegadores).
- **Precisión.** Estas solicitudes duran pocos milisegundos y `/proc` suele contar CPU en pasos de 10 ms. Con `/proc`, una CPU de 0 significa «por debajo de la resolución», no «sin trabajo».
- **Fallo.** En CUPS se usa una ruta inexistente. En las órdenes, un argumento inválido **de lectura**. Nunca se provoca un fallo en el servicio.
- **`escalado`** no aplica a estos servicios si no tienen una entrada cuyo tamaño se pueda variar. Se declara «no aplica»; no se fuerza.
- El cliente y el servidor no son el mismo proceso: B mide el servicio y, en nivel `ventana`, no separa otros clientes simultáneos.

**Recalibración.** Con la salida de F0 se completa en `CHECKLIST.md`: servicio, presente (sí/no), PID, usuario, canal, latencia mediana de 5 consultas y línea base. Si un servicio falta, se usa el siguiente de la tabla. Si faltan todos, se usa el servidor de Python. Todo cambio de umbral se propone al usuario antes de aplicarlo.

### 4.10 Niveles de tiempo (`--tiempo`)

El **fin** de la solicitud lo determina el destino (4.8). El plazo es solo la espera máxima: no alarga ni recorta una solicitud que termina a tiempo. El nivel indica la complejidad de la prueba:

| Nivel | Prueba | Factor | `respuesta` y `fallo` | `escalado` | Cota de `estabilidad` |
|---|---|---|---|---|---|
| `normal` | Ligera: consultas, archivos pequeños, pruebas obligatorias | ×1 | 10 s | 30 s | 60 s |
| `alto` | Media: archivos grandes, abrir una app, equipo lento | ×3 | 30 s | 90 s | 180 s |
| `muy-alto` | Pesada: compilar, comprimir archivos muy grandes, cálculos largos | ×6 | 60 s | 180 s | 360 s |

- En `estabilidad`, el plazo es `min(cota, factor × max(10 s, 3 × calibración))`. La solicitud de calibración usa la cota. Si la agota, el lote no empieza y se sugiere el nivel siguiente.
- Solo los 10 s de `normal` se apoyan en [S1]. Los factores, 30 s y las cotas son **decisión propia** y se recalibran en Lubuntu. Los niveles no tienen fuente y así se declara.
- **Guía.** Mide una vez la entrada más pesada (`T`) y usa el primer nivel con límite mayor o igual a `2 × T`. Si `muy-alto` no alcanza, reduce la entrada. TraceLite **no** sube el nivel solo.
- **No cambia** `--slo-ms`, la clase de latencia ni ningún criterio de alerta. Con `respuesta`, una acción de 20 s en `alto` termina, pero sigue siendo `slo_excedido`.
- El nivel no es criterio de igualdad de las referencias, porque no altera la medida. Las solicitudes cortadas son ERROR y quedan fuera de ellas.
- Al iniciar se imprime `[tracelite] modo=<m> tiempo=<nivel> plazo=<s> s`. Con `--plazo`, `tiempo` es `manual`.
- Peor caso, sin contar el cierre: `n × plazo`. 5 solicitudes de `estabilidad` en `muy-alto` suman 30 min, más 6 min de calibración. `Ctrl+C` cierra en orden y conserva lo completado.

## 5. Diseño eBPF

### 5.1 Herramientas

libbpf con CO-RE, todo en C. Paquetes sugeridos: `clang llvm libbpf-dev libelf-dev zlib1g-dev libcap2-bin` y `bpftool` (en Ubuntu, dentro de `linux-tools-$(uname -r)` o `linux-tools-common`). `vmlinux.h` se genera con `bpftool btf dump file /sys/kernel/btf/vmlinux format c` y **no** se versiona.

### 5.2 Programas (`src/bpf/trace.bpf.c`)

Usa `tp_btf/…`. Los tracepoints clásicos exigen leer `tracefs`, normalmente reservado a root.

| Programa | Acción |
|---|---|
| `tp_btf/sched_process_fork` (parent, child) | Si el tgid del padre está marcado y `child->pid == child->tgid`: marca al hijo y emite FORK |
| `tp_btf/sched_process_exec` (p) | Si está marcado: emite EXEC con `comm` |
| `tp_btf/sched_process_exit` (p) | Si está marcado y `p->pid == p->tgid`: emite EXIT. Incluye PID, código y hora |
| `tp_btf/sched_switch` (preempt, prev, next) | Acumula CPU por tgid marcado; guarda el instante de entrada de cada hilo marcado |

Declara solo los argumentos iniciales necesarios: versiones nuevas del kernel añaden argumentos al final. Verifica las firmas en el BTF local antes de escribir.

| Mapa | Tipo | Contenido |
|---|---|---|
| `marcados` | HASH, 10240 | tgid → número de solicitud |
| `cpu` | HASH, 10240 | tgid → ns acumulados (suma atómica) |
| `entrada` | HASH, 10240 | tid → ns de entrada a CPU |
| `eventos` | RINGBUF, 256 KiB | eventos hacia el usuario (U5) |
| `perdidos` | PERCPU_ARRAY, 1 | reservas fallidas |

Las marcas se borran en EXIT, **después** de que el espacio de usuario pueda leer la CPU: conserva la entrada `cpu` hasta `detener()`. Los tiempos de vida terminan con la salida del líder; un líder que muere antes que sus hilos es una limitación documentada. Los hilos que ya ejecutan al marcar una app se cuentan desde su próximo `sched_switch`.

Usa PIDs del namespace inicial. No se admiten contenedores ni WSL en la ruta eBPF.

### 5.3 Privilegios

`tracelite`, A y el programa real corren como usuario normal. **Nunca** se ejecuta todo con `sudo`: los navegadores suelen rechazar root. Solo B recibe capacidades:

```bash
sudo setcap cap_bpf,cap_perfmon=ep bin/servicio_b
getcap bin/servicio_b
```

Se repite tras cada recompilación. Si el sistema exige otra capacidad, informa el error exacto del verificador o de `bpf()`; no amplíes permisos sin preguntar. B no ejecuta programas ni abre rutas recibidas por red, y valida con `SO_PEERCRED` que el cliente tenga su mismo UID.

### 5.4 Niveles de atribución

| Nivel | Significado | Cuándo |
|---|---|---|
| `exacta` | El proceso desciende del que TraceLite lanzó para la solicitud; el kernel lo registró | Destino `programa` con eBPF |
| `conexion` | El kernel asoció la solicitud a los hilos que leyeron **su conexión TCP** y a lo que esos hilos crearon. El trabajo delegado a hilos o procesos que ya existían no se ve | Destino `backend` con eBPF |
| `ventana` | Actividad del servicio durante la solicitud, menos la línea base. No prueba la causa | Destino `app`, o cualquier destino con fuente `/proc` |

Nunca presentes `ventana` como certeza. `conexion` aplica el mismo principio que Beyla en el nivel del kernel y comparte sus límites [S6].

### 5.5 Seguimiento por conexión (destino `backend`, fase F7)

Se marca la **conexión**, no el proceso: el backend ya existía y atiende muchas solicitudes.

1. A abre la conexión TCP y lee su puerto local con `getsockname`.
2. A envía a B `MSG_INICIAR` con ese puerto. Agrega `int puerto_cliente` a `ctx_t`.
3. B guarda en el mapa `conexiones` (`puerto_cliente` → solicitud) y responde `MSG_LISTO`.
4. A envía la petición. Como lo hace después de `MSG_LISTO`, B ya registró la conexión antes de que existan datos.
5. `fexit/tcp_recvmsg` con retorno mayor que 0 (**no** `fentry`: el hilo del servidor puede entrar y bloquearse antes de que B registre el puerto, y la marca se perdería): si el socket pertenece a una conexión registrada (puerto remoto igual a `puerto_cliente` y puerto local igual al del backend, ambos en `127.0.0.1`), marca el hilo actual en el mapa `hilos_marcados` (tid → solicitud).
6. `sched_process_fork`: si el padre (tid) está marcado, el hijo hereda la marca.
7. `sched_switch`: acumula CPU por **solicitud** (clave `solicitud`, no tgid) cuando `prev` está en `hilos_marcados`. Generaliza el acumulador de F5.
8. Al recibir `MSG_TERMINAR`, B limpia sus entradas. `fin` es la última salida de CPU de un hilo marcado, acotada por el instante de `MSG_TERMINAR`.

Antes de programar, verifica en el equipo que `tcp_recvmsg` existe y admite `fexit` (`bpftool btf dump file /sys/kernel/btf/vmlinux`, `/proc/kallsyms`) y los nombres exactos de los campos del socket. Si no se puede enganchar, el destino cae a `ventana` con un aviso.

Límites, que van al informe: solo IPv4 en `127.0.0.1`; sin TLS ni HTTP/2; solicitudes secuenciales; el trabajo que el backend delega a hilos ya existentes (grupos de hilos, bucles de eventos) no se atribuye; se muestran los eventos perdidos.

## 6. Fases

### F0 — Base y diagnóstico
- [ ] Crear `CHECKLIST.md` con R1–R5, P1–P3, E1, U1–U11 y las fases.
- [ ] `Makefile`: agregar `include/*.h` como dependencia (hoy cambiar `span_t` no recompila) y objetivos `ebpf`, `check`.
- [ ] `id_hex`: sustituir `rand()` por `getrandom()`; con 7 días de historial, la semilla `pid ^ time` puede repetir IDs.
- [ ] `escribir_exacto` y `leer_exacto`: reintentar ante `EINTR`.
- [ ] **Rutas.** Hoy `./bin/collector` y `./bin/monitor_pid` dependen del directorio actual. Función `ruta_hermana(nombre, out, out_sz)` en `common.c`: `readlink("/proc/self/exe")`, quita el último componente y añade el nombre; comprueba error y truncamiento. Úsala en los tres `execv` y en los binarios nuevos. No uses `argv[0]`.
- [ ] **Socket.** Hoy `collector` hace `unlink` y `bind` sobre `/tmp/tracelite.sock`: una segunda ejecución reemplaza el socket de la primera. Crea el directorio de 4.3 con `crear_dir_privado(out, out_sz)` y pasa `<dir>/spans.sock` por `argv`, como hoy. Elimina el valor fijo, también en `collector`.
- [ ] **Cierre.** Hoy solo resumen instala `manejar_senal`. Instálalo también en instantánea, antes de lanzar hijos, y trata `EINTR` en `waitpid`. Ante `SIGINT` o `SIGTERM`, o si un hijo falla al iniciar, `collector` siempre debe terminar: envía `__FIN__` o, si no es posible, `SIGTERM` a los hijos que lanzó `tracelite`.
- [ ] Corregir en README la ruta `pruebas/` → `docs/pruebas/`.
- [ ] `docs/pruebas/diagnostico_ebpf.sh`: informa, sin sudo, kernel, BTF, clang, bpftool, libbpf, setcap y `kernel.unprivileged_bpf_disabled`.

- [ ] `docs/pruebas/detectar_servicios.sh`: de solo lectura y sin sudo. Ejecuta `ss -ltn`, `ss -lx`, `systemctl is-active` y `systemctl show -p MainPID` para la tabla de 4.9, y mide 5 consultas de cada servicio presente. Informa los ausentes sin fallar.

**Aceptación:** `detectar_servicios.sh` termina con código 0 y su salida se entrega al usuario para recalibrar 4.9; `make clean && make && make test` sin advertencias; `./bin/tracelite --help` y el observador siguen funcionando; salida del diagnóstico entregada al usuario. Además, `docs/pruebas/prueba_base.sh` comprueba:

- desde `/`, con ruta absoluta, instantánea y `--resumen -d 2` sobre un `sleep` propio funcionan;
- dos ejecuciones simultáneas sobre procesos distintos dan cada una su propia tabla;
- el directorio tiene permisos `700` durante la ejecución;
- tras el fin normal y tras `SIGINT` y `SIGTERM` enviados solo a `tracelite`, en ambas formas, no quedan directorio, sockets ni hijos;
- un `XDG_RUNTIME_DIR` demasiado largo da error claro y no bloquea;
- `git diff` no muestra comentarios existentes alterados, salvo los que dejaron de ser ciertos (se listan).

### F1 — Núcleo compartido
- [ ] Ampliar `span_t` (4.1) y `tests/test_span.c`.
- [ ] Extraer de `collector.c` a `src/tabla.c` la impresión reutilizable, sin cambiar su salida.
- [ ] Crear `protocolo.h` y `fuente.h`.

**Aceptación:** `make test` reporta 288 bytes; el observador imprime las mismas columnas que antes.

### F2 — Solicitudes A/B con `/proc`
- [ ] `src/servicio_a.c`, `src/servicio_b.c`, `src/fuente_proc.c`, `src/solicitudes.c` (Collector Lite en el proceso principal). `tracelite.c` elige entre observación y solicitud.
- [ ] Reutilizar lo listado en 2.2; mueve a un módulo común las funciones compartidas en vez de copiarlas.
- [ ] Parser de la sección 3: instantánea, `--resumen`, `-n`, `--`, `--lote`, `--plazo` y rechazo de combinaciones inválidas.
- [ ] Secuencia 4.4, plazos y cierre.
- [ ] `tests/test_solicitudes.sh`.

**Aceptación:** `-- sha256sum <archivo>` da 1 traza y 2 spans; `--resumen -n 5 -- sha256sum <archivo>` da 5 trazas y 10 spans; la observación conserva sus columnas; se cumplen los invariantes; `ps --forest` muestra A y B como hijos de `tracelite`; sin zombis ni sockets restantes; comando inexistente → ERROR; `kill -9` a B no bloquea; `sleep 5 --plazo 1` (opción antes de `--`) termina como `plazo_vencido`.

### F3 — Persistencia y resumen
- [ ] `src/almacen.c` (4.5), `--historial`, `--historial --trace`, retención y resumen final.
- [ ] Guardar también la observación (`tipo` = `observacion`).
- [ ] `tests/test_almacen.c`: escape, línea truncada, recarga y retención con fechas fabricadas en un directorio temporal.

**Aceptación:** tras terminar, el archivo tiene 10 líneas válidas (`python3 -m json.tool` por línea); una segunda ejecución deja 20; `--historial --trace` reconstruye el árbol en un proceso nuevo; la retención no toca archivos ajenos.

### F4 — Anomalías, alertas y pruebas obligatorias
- [ ] Criterio 4.6 y alerta inmediata.
- [ ] `docs/pruebas/preparar_datos.sh`: crea `datos/normal.bin` y `datos/pesado.bin` (10 veces mayor), **calibrados** para que el caso normal dure al menos 200 ms. `datos/` va en `.gitignore`.
- [ ] `docs/pruebas/caso_normal.sh` (4 iguales), `caso_anomalo.sh` (4 normales y 1 pesada, en ese orden), `make check`.

**Aceptación:** el caso normal da `alertas=0` en 10 repeticiones, cada una con historial vacío; el anómalo da 1 alerta en 10 de 10, con servicio y Trace ID; ambos spans están en disco. Si hay falsas alertas, reporta las medidas; no ajustes el umbral en silencio. **En este punto el enunciado queda cubierto en código.**

### F5 — eBPF (destino `programa`)
Requisitos: F4 terminada, diagnóstico correcto, visto bueno del usuario.
- [ ] `src/bpf/trace.bpf.c`, `src/fuente_ebpf.c`, `make ebpf`. `make` sin dependencias eBPF debe seguir funcionando.
- [ ] `--fuente auto|ebpf|proc`.
- [ ] `docs/pruebas/comparar_fuentes.sh`: un programa que crea 50 hijos muy cortos, medido con ambas fuentes.

**Aceptación:** eBPF registra todos los hijos, con `perdidos=0`, y se documenta cuántos vio `/proc`; `make check` pasa con `--fuente ebpf`; sin capacidades, `auto` cae a `/proc` y `ebpf` falla con un mensaje claro.

### F6 — Destino `backend` con `/proc` (nuevo y vivo)
Requisito: F4 terminada. Es la fase principal del punto de vista «servicio vivo».
- [ ] `docs/pruebas/backend_demo.py`: `ThreadingHTTPServer` en `127.0.0.1`, puerto por `argv`, sin dependencias externas, sin `shell` y sin escribir archivos. Rutas: `/hash?mb=N` (SHA-256 sobre N MiB generados en memoria; costo lineal en N), `/espera?ms=N` (espera sin CPU), `/error` (responde 500) y 404 para el resto. Opción `--mostrar-cabeceras` imprime en `stderr` el `traceparent` recibido.
- [ ] Cliente HTTP/1.0 en `servicio_a.c` (4.8): conexión con plazo, envío, lectura hasta el fin, lectura del código y encabezado `traceparent`.
- [ ] `--backend`, `--lanzar-backend`, `--ruta`, `--linea-base` (4.8) y `--unidad` (4.9).
- [ ] `fuente_proc` para servicios: resuelve el PID del backend con `--backend-pid` o, si falta, con `/proc/net/tcp` y `/proc/*/fd`; si no puede, falla con un mensaje claro.

**Aceptación:**
- Backend nuevo: 5 solicitudes, 10 spans, un único PID de backend durante las 5, y sin procesos ni sockets restantes al final.
- Backend vivo: 5 solicitudes, y el backend conserva el mismo PID y sigue respondiendo después.
- `/error` da ERROR `http_500`; un puerto cerrado da ERROR sin colgarse; el `traceparent` impreso por el backend coincide con el `trace_id` y el `span_id` de A.

### F7 — eBPF por conexión
Requisitos: F5 y F6 terminadas, y verificación de 5.5.
- [ ] Programas, mapas y `puerto_cliente` de 5.5; acumulador por solicitud.
- [ ] Nivel `conexion` en `span_t.atribucion`.

**Aceptación:**
- `/espera?ms=300`: `dur` cerca de 300 ms y `cpu_ms` menor que 20 ms.
- `/hash?mb=64`: `cpu_ms` dentro de ±30 % de `dur`.
- Prueba de ruido: mientras corre la solicitud, otro cliente (`curl` en bucle) consulta el mismo backend. Reporta `cpu_ms` con `conexion` y con `ventana`: el primero cambia poco y el segundo aumenta. Si no ocurre, reporta las medidas y no ajustes nada en silencio.

### F8 — Destino `app` (viva y nueva) con línea base
- [ ] `--app NOMBRE|PID` (reutiliza `buscar_pid_por_nombre`), `--lanzar-app`, orden después de `--`, y fin por inactividad (4.8) o `--fin orden` (4.9).
- [ ] Servicio vivo: nunca se le envían señales. Servicio nuevo: se detiene al final.

- [ ] `docs/pruebas/demo_servicios_so.sh`: 5 solicitudes a cada servicio de 4.9 que F0 detectó.

**Aceptación:** 5 solicitudes sobre una app viva o un servicio del SO producen 5 trazas con atribución `ventana` y `fondo=` en `msg`; la app conserva su PID. Es una demostración: no aprueba ni reprueba el avance.

### F9 — Modos y matriz de pruebas
- [ ] `--modo`, generadores y criterios de 4.8; `--slo-ms`, `--entradas`, `--tamanos`, `--fallo-en`; calibración en `estabilidad`; clase de latencia en `msg` de A.
- [ ] `--tiempo` (4.10): plazos, mensajes, claves `tiempo` y `plazo_s`.
- [ ] `docs/pruebas/matriz_modos.sh` y `make check-modos`, con la matriz de 4.8 para `programa` y `backend` (nuevo y vivo).

**Aceptación** (usa `backend_demo.py`; cada caso se repite 10 veces con historial vacío):
- `respuesta`: `/espera?ms=50` da 0 alertas; con `--slo-ms 20` da 5 alertas `slo_excedido`.
- `estabilidad`: 5 solicitudes idénticas dan 0 alertas; con 4 de `mb=8` y 1 de `mb=80` (`--entradas`) da 1 alerta `latencia_alta`.
- `escalado`: `mb` = 4, 8, 16, 32, 64 con `--tamanos 1,2,4,8,16` da 0 alertas.
- `fallo`: `--fallo-en 5` da 1 alerta `error_proceso`.
- `--tiempo` (se ejecuta una vez): una instantánea a `/espera?ms=15000` en `respuesta` se corta a los 10 s con `normal`; con `alto` termina y da `slo_excedido`. `--tiempo` con `--plazo` termina con código 2.
- Instantánea: `--modo escalado` y `--modo fallo` sin `--resumen` terminan con código 2.
- Si hay falsas alertas, reporta las medidas; no cambies umbrales en silencio.

### F10 — Documentación
- [ ] README: instalación, comandos, privilegios, formas, destinos, modos, niveles de tiempo, limitaciones; tamaño de `span_t` actualizado.
- [ ] Diagrama de procesos e IPC con los dos puntos de vista.
- [ ] Informe: descripción, diagrama, formato del span, persistencia justificada, anomalía y criterio, tabla de plazos con sus fuentes y las decisiones propias marcadas, niveles de atribución y sus límites, evidencias, bibliografía (sección 10). Estilo: sección 2 de `PLAN_INFORME_AVANCE.md`.
- [ ] Verificar la fuente [S3] contra la especificación oficial de OpenTelemetry y [S7] contra el texto de W3C antes de citarlas.
- [ ] `CHECKLIST.md` con cumplimiento, evidencia y limitaciones.
- [ ] Guion del video: 5 min de explicación y 5 de demostración (caso normal y anómalo, persistencia tras cerrar, un backend vivo atendiendo solicitudes, eBPF frente a `/proc`, una app viva).

`.gitignore` excluye `docs/*INFORME*` y `docs/diagramas/`. Pregunta al usuario si deben versionarse; entrega el informe por la vía que indique.

## 7. Riesgos y límites

| Riesgo | Respuesta |
|---|---|
| El profesor no acepta eBPF | Entregar F0–F4; citar eBPF como trabajo futuro |
| El profesor no acepta A/B como entrega y observación | Consultar **antes de F2** con el diagrama de la sección 2 |
| El BTF o las firmas difieren en kernel 7.0 | Verificar en el equipo; respaldo `/proc` |
| Se pierden eventos | Se cuentan y se muestran; nunca se ocultan |
| Falsas alertas en apps vivas | Pruebas obligatorias solo con programas deterministas |
| Procesos que se desprenden (daemon) | eBPF conserva la marca; tras 2 s se anotan como vivos |
| Operaciones internas (una función, una consulta HTTP) | Fuera de alcance: exige uprobes o instrumentación específica por programa |
| Backend de bucle de eventos o con grupos de hilos | `conexion` pierde el trabajo delegado a otros hilos; se declara y se compara con `ventana` |
| Apps con trabajo de fondo propio | Línea base y nivel `ventana`; nunca se presenta como certeza |
| 5 solicitudes no bastan para percentiles | Mediana y máximo; el informe lo declara [S5] |
| Cerrar por error un servicio vivo | Regla 12: nunca se envían señales a lo que TraceLite no inició |
| La interfaz retrasa lo obligatorio | Extra y proceso separado; G1 solo después de F4 |
| Prueba más larga que el límite | Reducir la entrada o elegir otro `--tiempo` (4.10); nunca ocultar el corte |
| Servicios del SO ausentes, inactivos o bajo demanda | Detección en F0, calentamiento y sustituto (4.9) |
| HTTPS, HTTP/2 y backends remotos | Fuera de alcance; el destino `backend` es HTTP/1.0 en `127.0.0.1` |

## 8. Interfaz gráfica (`tracelite-gui`)

Es un **extra**: el enunciado se cumple en la terminal. Requiere visto bueno (D5). Toda acción de la interfaz existe como comando.

### 8.1 Diseño

- **Tecnología:** C con GTK 4 y Cairo. Verifica la versión con `pkg-config --modversion gtk4`; el paquete esperado es `libgtk-4-dev`. `make gui` es opcional; `make` funciona sin GTK.
- **Proceso separado.** Lee los JSONL y lanza `tracelite`. No escribe trazas, no carga eBPF, no usa root. Si falla, la recolección no se afecta.
- **Lectura.** Reutiliza el lector de `almacen.c` en modo solo lectura: **no** aplica retención ni borra. Vigila el directorio con `GFileMonitor`, lee desde el último desplazamiento y conserva la línea incompleta hasta que llegue su fin. Detecta segmentos nuevos y borrados.
- **En vivo** significa al completarse cada solicitud: los spans se guardan entonces, no durante su ejecución.
- Referencia general: los visores de trazas. No copies la interfaz, la marca ni los iconos de Grafana.

### 8.2 Vistas (G1)

1. **Lista de trazas:** hora, modo, destino, operación, duración, estado, alerta. Filtros por texto o Trace ID, estado, modo y alerta.
2. **Cascada:** un rectángulo por span. `x = (ini - ini_traza) × escala` y `ancho = dur × escala`, con un ancho mínimo visible; B va con sangría bajo A. Usa `ini_ns` dentro de una traza y `*_real_ns` entre trazas.
3. **Detalle:** todos los campos, con `atribucion` siempre visible y su significado (`ventana` no prueba causa).
4. **Tabla de procesos** para `tipo` = `observacion`.
5. **Alertas:** resaltadas con texto además de color; abren su traza.

### 8.3 Formulario (G2)

Un solo formulario que cambia según forma, destino y modo. El comando resultante siempre está visible.

| Selección | Campos |
|---|---|
| Forma | Observación, Instantánea o Resumen |
| Observación | PID o nombre. Opción «durante un tiempo» (`--resumen`): duración (`-d`) e intervalo |
| Destino `programa` | Programa y argumentos |
| Destino `backend` | Vivo (dirección, unidad) o nuevo (comando); ruta |
| Destino `app` | Viva (nombre, PID o unidad) o nueva (comando); orden; fin |
| Modo | Los admitidos por la forma (sección 3) y sus opciones propias (4.8) |
| Tiempo | `normal`, `alto`, `muy-alto`, con los segundos de 4.10. Oculto en observación |

- Los campos salen de una **tabla estática en C** (opción, etiqueta, tipo, mínimo, máximo, cuándo aplica). Lista: `GtkDropDown`; número: `GtkSpinButton`; texto: `GtkEntry`. Una opción de tipo conocido se añade con una fila; una vista nueva requiere código.
- `tracelite` valida siempre; la interfaz solo ayuda. Su mensaje de error se muestra tal cual.
- **Ejecutar:** `GSubprocess` con un **arreglo de argumentos**, nunca shell. Cada argumento de la orden es un campo propio.
- **Detener:** `SIGINT` solo al `tracelite` que la interfaz lanzó. El cierre no puede depender de que A y B reciban `Ctrl+C` por la terminal. Nunca se señala al programa observado.
- Una ejecución a la vez desde la interfaz. Cerrar la ventana durante una pide confirmación.

### 8.4 Fases

**G1 — Visor.** Requisitos: F4 y D5. **Aceptación:** muestra las mismas trazas, spans y alertas que `--historial`; una traza nueva aparece sin reiniciar; tolera líneas corruptas y directorio vacío; los archivos no cambian mientras solo se usa el visor.

**G2 — Formulario.** Requisitos: G1 y F9. **Aceptación:** por cada combinación válida, los argumentos coinciden con los de la sección 3; un texto como `; touch /tmp/x` llega como argumento literal; Detener no deja procesos ni sockets propios y un servicio vivo conserva su PID.

Si falta tiempo, G1 se entrega sin G2. Fuera de alcance: TraceQL, paneles editables, usuarios, acceso remoto.

## 9. Decisiones del usuario

| # | Decisión | Bloquea | Por defecto |
|---|---|---|---|
| D1 | Confirmar con el profesor que A/B como entrega y observación es válido | F2 | Esperar |
| D2 | Confirmar eBPF con el profesor | F5 | Esperar |
| D3 | Programa de la demostración obligatoria | F4 | `gzip` |
| D4 | App viva de la demostración | F6 | Firefox |
| D5 | Autorizar el inicio de G1 y G2 (GTK 4 + Cairo) | G1, G2 | Esperar; F0–F10 no dependen de ello |
| D6 | Recalibrar 4.9 con la salida de `detectar_servicios.sh` en Lubuntu | F6 y F8 (solo la demostración) | CUPS y systemd-resolved; pruebas con `backend_demo.py` |
| D7 | Aprobar los plazos y umbrales marcados «decisión propia» | F9 | Aplicar los valores de 4.8 |
| D8 | Confirmar con el profesor que los servicios vivos y el seguimiento por conexión caben en el alcance | F6 | Esperar |
| D9 | Integrar las solicitudes en instantánea (1) y resumen (5), sin subcomando | F2 | **Decidido** por el usuario |
| D10 | Niveles `normal`, `alto`, `muy-alto` y sus factores | F9 | Niveles pedidos por el usuario; nombres y factores: valores de 4.10, por recalibrar |

## 10. Fuentes

El agente no inventa fuentes ni valores. Todo plazo o umbral que no tenga fuente aquí es una **decisión propia** y se escribe así en el informe.

| ID | Fuente | Qué respalda | Nota |
|---|---|---|---|
| S1 | NN/g, «Response Times: The 3 Important Limits»: <https://www.nngroup.com/alertbox/timeframes.html> (video: <https://www.nngroup.com/videos/3-response-time-limits-interaction-design/>) | Límites de 0,1 s, 1 s y 10 s; plazo de 10 s | Nielsen aclara que no son umbrales absolutos |
| S2 | web.dev, Interaction to Next Paint: <https://web.dev/articles/inp> | Bueno ≤ 200 ms, pobre > 500 ms (percentil 75) | Mide interacciones de páginas web; aquí es solo referencia para acciones de apps |
| S3 | Valores por defecto del procesador por lotes de OpenTelemetry (5 s de retraso, 30 s de exportación): <https://observability.opensearch.org/docs/send-data/data-pipeline/batching/> | Espera de 5 s por spans pendientes | Fuente secundaria. **Verificar** en la especificación de OpenTelemetry antes de citar |
| S4 | Google SRE Book, capítulos 21 y 22: <https://sre.google/sre-book/handling-overload/>, <https://sre.google/sre-book/addressing-cascading-failures/> | Plazos del cliente y trabajo desperdiciado tras el plazo | Respaldo cualitativo; no da valores numéricos |
| S5 | Google Cloud, «Building good SLOs»: <https://cloud.google.com/blog/products/gcp/building-good-slos-cre-life-lessons/> | Contar solicitudes bajo un umbral; evitar la media; p95 y p99 | Base de `slo_excedido` y de la limitación de 5 muestras |
| S6 | Grafana Beyla, trazas distribuidas: <https://grafana.com/docs/beyla/latest/distributed-traces>; FOSDEM 2024: <https://archive.fosdem.org/2024/events/attachments/fosdem-2024-3499-implementing-distributed-traces-with-ebpf/slides/22741/FOSDEM_2024-Implementing_distributed_traces_wit_ruFpAqa.pdf> | Seguimiento de solicitudes en el kernel (`accept`, `tcp_recvmsg`, `tcp_sendmsg`) y sus límites | Beyla no crea spans de funciones internas |
| S7 | W3C, Trace Context: <https://www.w3.org/TR/trace-context/> | Formato del encabezado `traceparent` | No se consultó en esta revisión; **verificar** |
| S8 | OpenTelemetry, Traces: <https://opentelemetry.io/docs/concepts/signals/traces/>; Grafana Tempo: <https://grafana.com/docs/tempo/latest/> | Modelo conceptual (referencias del enunciado) | |
