# Guion del Video de Entrega — TraceLite (10 minutos)

## Parte 1: Explicación técnica (5 minutos)

1. **Introducción y problema (0:00 – 1:00):**
   - Qué es TraceLite: trazabilidad distribuida sobre procesos reales de Linux sin instrumentar los binarios observados.
   - Los dos puntos de vista: programas nuevos (`gzip`, comandos arbitrarios) y servicios vivos ya en ejecución (`backend_demo.py`, `cups.service`, `systemd-resolved`).
2. **Arquitectura de procesos e IPC (1:00 – 2:30):**
   - Mostrar `docs/diagramas/arquitectura_ipc.md`.
   - `tracelite` (Collector Lite) crea directorio privado `0700`, lanza `servicio_a` (entrega) y `servicio_b` (observador) con `fork()` + `execv()`, comunica solicitudes/contexto/spans vía Sockets UNIX (`AF_UNIX`) e hilos (`pthread`).
   - Barrera `pipe2` antes de `execvp` para no perder eventos iniciales.
3. **Núcleo eBPF vs respaldo `/proc` (2:30 – 3:45):**
   - Explicar `trace.bpf.c` (`sched_process_fork/exec/exit`, `sched_switch`, `fexit/tcp_recvmsg`), mapas `HASH` y `BPF_MAP_TYPE_RINGBUF`.
   - Explicar los tres niveles de atribución: `exacta`, `conexion` y `ventana` (y el principio de mínimo privilegio con `setcap cap_bpf,cap_perfmon=ep` solo en `servicio_b`).
4. **Persistencia y detección de anomalías (3:45 – 5:00):**
   - Almacenamiento JSONL diario (`AAAA-MM-DD.jsonl`) con `fsync` y retención de 7 días.
   - Criterio estadístico Leave-One-Out ($\mu + 2\sigma$, $1.5\mu$, $+10\text{ ms}$) y los 4 modos (`respuesta`, `estabilidad`, `escalado`, `fallo`).

---

## Parte 2: Demostración en vivo en Lubuntu (5 minutos)

1. **Compilación y pruebas base (5:00 – 5:45):**
   ```bash
   make clean && make && make ebpf && make test
   getcap bin/servicio_b
   ```
2. **Caso normal y caso anómalo (5:45 – 7:00):**
   ```bash
   bash docs/pruebas/caso_normal.sh
   bash docs/pruebas/caso_anomalo.sh
   ```
   Mostrar la alerta inmediata `[ALERTA] ... tipo=latencia_alta` y el resumen final.
3. **Persistencia tras cerrar el programa (7:00 – 7:45):**
   ```bash
   ./bin/tracelite --historial
   ./bin/tracelite --historial --trace <trace_id_anomalo>
   ```
4. **eBPF vs `/proc` con 50 hijos efímeros (7:45 – 8:30):**
   ```bash
   bash docs/pruebas/comparar_fuentes.sh
   ```
   Destacar `n_procs=1` (`ventana`) frente a `n_procs=51`, `perdidos=0` (`exacta`).
5. **Backend vivo atendiendo solicitudes y prueba de ruido (8:30 – 9:30):**
   ```bash
   bash tests/test_ebpf_conexion.sh
   ```
   Mostrar cómo `conexion` mantiene `cpu_ms ~ 0.5 ms` en `/espera?ms=300` mientras otro cliente hace ruido, frente a `ventana` (`~220 ms`).
6. **App / servicio del SO vivo (9:30 – 10:00):**
   ```bash
   bash docs/pruebas/demo_servicios_so.sh
   ```
   Mostrar que `cups.service` y `systemd-resolved.service` conservan su PID original y registran atribución `ventana`.
