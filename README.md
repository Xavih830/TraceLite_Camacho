# TraceLite — Trazabilidad Distribuida de Procesos en GNU/Linux

Sistema liviano de trazabilidad en C (`-std=gnu11`) sobre **GNU/Linux** que observa árboles reales de procesos vivos del sistema operativo mediante `/proc`, crea procesos con `fork()` y `execv()`, comunica eventos mediante **Sockets UNIX (`AF_UNIX`)** (*message passing*), atiende clientes concurrentes con **hilos (`pthread`)** y reconstruye trazas jerárquicas en cascada adaptables al ancho de la terminal.

---

## 1. Estructura del Proyecto

```text
TraceLite_Camacho/
├── Makefile
├── README.md
├── include/
│   ├── common.h          # Utilidades de tiempo, E/S exacta, IDs y spans
│   └── span.h            # Estructura fija span_t (264 bytes)
├── src/
│   ├── common.c          # Implementación común (CLOCK_MONOTONIC, sockets/pipes, spans)
│   ├── tracelite.c       # Orquestador principal: descubre jerarquía por PPID y lanza monitores
│   ├── monitor_pid.c     # Agente observador por PID (/proc/<pid>/stat, cmdline, wchan)
│   └── collector.c       # Colector concurrente AF_UNIX + pthread + tabla adaptable (TIOCGWINSZ)
├── tests/
│   └── test_span.c       # Pruebas unitarias de spans y E/S exacta
└── bin/                  # Binarios compilados (generado por make)
```

---

## 2. Compilación y Pruebas

### Prerrequisitos
- **GNU/Linux** (nativo como Lubuntu/Ubuntu, o WSL2).
- `gcc >= 9` con soporte `pthread` y `GNU Make`.

### Comandos
```bash
# Compilar bin/tracelite, bin/collector y bin/monitor_pid
make

# Ejecutar las pruebas unitarias de spans y E/S
make test

# Ejecutar TraceLite con descubrimiento automático de árbol de procesos
make run

# Limpiar binarios
make clean
```

---

## 3. Modos de Uso de `./bin/tracelite`

### Modo árbol automático / por nombre o PID (predeterminado)
Escanea `/proc` leyendo `PID` y `PPID`, descubre el subárbol completo desde un nombre de proceso o PID raíz, lanza un proceso `./bin/monitor_pid` en paralelo por cada nodo usando `fork()` + `execv()`, y envía los spans al colector vía `/tmp/tracelite.sock`:
```bash
./bin/tracelite
./bin/tracelite <PID_RAIZ | NOMBRE>
# Ejemplos intuitivos por nombre en Lubuntu:
./bin/tracelite antigravity
./bin/tracelite chrome
./bin/tracelite postgres
```

### Modo `--resumen` (captura continua con cierre automático en 60 s)
Monitorea la aparición y finalización de subprocesos bajo un proceso o PID raíz durante **60 segundos** (con contador en vivo en terminal) y al finalizar imprime la tabla consolidada:
```bash
./bin/tracelite --resumen antigravity
# Personalizar duración en segundos (-d) o intervalo en ms (--interval):
./bin/tracelite --resumen -d 30 antigravity
./bin/tracelite --resumen -d 20 chrome
```

---

## 4. Arquitectura e IPC

- **Creación de procesos (`fork` + `execv`):** `./bin/tracelite` lanza `./bin/collector` y múltiples `./bin/monitor_pid` en paralelo, recolectando todos los estados con `waitpid()` para evitar procesos *zombie*.
- **Paso de mensajes (*Message Passing*) con Sockets UNIX (`AF_UNIX`, `SOCK_STREAM`):** cada observador se conecta a `/tmp/tracelite.sock` y envía estructuras `span_t` de tamaño fijo.
- **Concurrencia en el colector (`pthread` + `pthread_mutex_t`):** `./bin/collector` crea un hilo por conexión entrante y protege el arreglo dinámico de spans con un *mutex*.
- **Visualización adaptable (`TIOCGWINSZ`):** calcula dinámicamente el ancho de las columnas (`INICIO`, `FIN`, `SPAN_ID`, `PARENT_ID`, `SERVICIO`, `OPERACION`, `DURACION`, `CASCADA`, `PID`, `ESTADO`) según el tamaño real de la terminal.
