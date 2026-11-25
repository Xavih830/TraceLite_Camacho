# TraceLite

TraceLite es una herramienta de trazabilidad distribuida y observación de procesos reales en GNU/Linux. Implementa una arquitectura maestro-servicios (`tracelite` como Collector Lite, `servicio_a` como entrega y `servicio_b` como observador) comunicados por Sockets UNIX (`AF_UNIX`), hilos (`pthread`), persistencia diaria JSONL y seguimiento en kernel mediante eBPF CO-RE (con respaldo automático en `/proc`).

---

## 1. Estructura del proyecto

- `Makefile`: compila los binarios base (`make`), soporte eBPF (`make ebpf`) y pruebas (`make test`, `make check`, `make check-modos`).
- `include/span.h`: define `span_t` (**288 bytes**).
- `include/protocolo.h`, `include/fuente.h`, `include/almacen.h`, `include/tabla.h`, `include/solicitudes.h`, `include/ebpf_eventos.h`, `include/common.h`: contratos e interfaces compartidas.
- `src/tracelite.c`: punto de entrada, parser CLI, modo observación e inicio de Collector Lite.
- `src/solicitudes.c`: Collector Lite en el proceso principal, evaluación de anomalías/modos y persistencia.
- `src/servicio_a.c`: ejecuta programas con barrera (`pipe2`) o envía peticiones HTTP/1.0 con `traceparent`.
- `src/servicio_b.c`: observa mediante `fuente_ebpf` o `fuente_proc` y calcula línea base.
- `src/bpf/trace.bpf.c` y `src/fuente_ebpf.c`: trazado en kernel (`tp_btf/sched_*` y `fexit/tcp_recvmsg`) con `BPF_MAP_TYPE_RINGBUF`.
- `src/fuente_proc.c`: muestreo periódico cada 20 ms en `/proc` por `PID + starttime`.
- `src/almacen.c`: almacenamiento JSONL append-only por día (`AAAA-MM-DD.jsonl`) con retención de 7 días.
- `src/collector.c`, `src/monitor_pid.c`, `src/tabla.c`, `src/common.c`: modo observación e impresión jerárquica.
- `docs/pruebas/`: scripts de calibración, diagnóstico eBPF, servicios del SO y matriz de pruebas.

---

## 2. Compilación e instalación

### Compilación estándar (sin dependencias eBPF)
```bash
make
make test
```

### Compilación con eBPF CO-RE (Ubuntu/Lubuntu 24.04+)
Instala `clang`, `llvm`, `libbpf-dev`, `libelf-dev`, `zlib1g-dev` y `bpftool`, compila eBPF y otorga capacidades **únicamente** a `bin/servicio_b`:
```bash
make ebpf
sudo setcap cap_bpf,cap_perfmon=ep bin/servicio_b
getcap bin/servicio_b
```
*(Repite `setcap` tras cada recompilación de `bin/servicio_b`. Nunca ejecutes `tracelite` ni apps observadas con `sudo`.)*

---

## 3. Modos de uso

### Observación (sin solicitudes)
```bash
./bin/tracelite [PID | NOMBRE]
./bin/tracelite --resumen -d 30 --interval 150 antigravity
```

### Solicitudes: Formas, Destinos, Modos y Niveles de tiempo
- **Formas:** instantánea (1 solicitud, por defecto) o `--resumen` (5 solicitudes o `-n N`).
- **Destinos:**
  - `programa` (nuevo): `./bin/tracelite --resumen -- gzip -kfc datos/normal.bin`
  - `backend` (vivo o nuevo):
    ```bash
    ./bin/tracelite --resumen --modo respuesta --backend 127.0.0.1:8000 --ruta "/hash?mb=8"
    ./bin/tracelite --resumen --lanzar-backend "python3 docs/pruebas/backend_demo.py 8000" --ruta "/hash?mb=8"
    ```
  - `app` (viva o nueva):
    ```bash
    ./bin/tracelite --resumen --modo respuesta --app firefox -- firefox --new-tab https://example.com
    ./bin/tracelite --resumen --app systemd-resolved --unidad systemd-resolved.service --fin orden -- resolvectl query localhost
    ```
- **Modos (`--modo`):** `respuesta` (`--slo-ms`), `estabilidad` (`latencia_alta`), `escalado` (`--entradas`, `--tamanos`), `fallo` (`--fallo-en`).
- **Niveles de tiempo (`--tiempo`):** `normal` (×1: 10s/30s/60s), `alto` (×3), `muy-alto` (×6), o `--plazo SEG`.
- **Fuente (`--fuente`):** `auto` (eBPF si hay capacidades, con respaldo a `/proc`), `ebpf`, `proc`.
- **Historial y retención:**
  ```bash
  ./bin/tracelite --historial
  ./bin/tracelite --historial --trace <trace_id>
  ```

---

## 4. Pruebas y verificación

```bash
make test         # pruebas unitarias (span_t = 288 bytes y almacen JSONL)
make check        # test_solicitudes.sh, caso_normal.sh y caso_anomalo.sh
make check-modos  # matriz completa de modos y niveles de tiempo
```

---

## 5. Limitaciones conocidas

- **eBPF por conexión (`conexion`):** soporta IPv4 en `127.0.0.1` sobre HTTP/1.0 sin TLS/HTTP2; el trabajo delegado a hilos ya existentes en el servidor (thread pools previos) no se atribuye a la conexión entrante.
- **Respaldo `/proc` (`ventana`):** muestrea cada 20 ms y lee CPU en ticks de 10 ms; puede omitir procesos muy cortos (<20 ms) y no separa tráfico concurrente de otros clientes.
- **Muestras pequeñas:** lotes de 5 solicitudes no permiten calcular percentiles `p95`/`p99`; se usan media/desviación estándar (`estabilidad`) y mediana (`escalado`).
- **Contenedores/WSL:** la ruta eBPF requiere kernel Linux con BTF y namespace de PID inicial.
