# TraceLite

TraceLite mide la trazabilidad de procesos y solicitudes en GNU/Linux. El sistema usa `fork()`, `execv()`, Sockets UNIX (`AF_UNIX`), hilos (`pthread`), registros JSONL y eBPF CO-RE (con respaldo en `/proc`).

---

## 1. Estructura del proyecto

- `Makefile`: compila los binarios y ejecuta las pruebas.
- `README.md`: describe la compilación y el uso.
- `include/`: cabeceras compartidas (`span.h`, `protocolo.h`, `fuente.h`, `almacen.h`, `solicitudes.h`, `tabla.h`, `ebpf_eventos.h`, `common.h`).
- `src/`: código fuente de `tracelite`, `collector`, `monitor_pid`, `servicio_a`, `servicio_b`, `almacen`, `solicitudes`, `tabla`, `fuente_proc`, `fuente_ebpf` y `bpf/trace.bpf.c`.
- `tests/`: pruebas unitarias en C (`test_span.c`, `test_almacen.c`) y de integración (`test_ebpf_conexion.sh`).
- `pruebas/`: scripts de prueba para servicios del sistema, carga y diagnóstico eBPF.

---

## 2. Requisitos

- GNU/Linux con `gcc`, `make`, `pthread` y `libm`.
- Para soporte eBPF (opcional): `clang`, `llvm`, `bpftool`, `libbpf-dev`, `libelf-dev` y `zlib1g-dev`.

---

## 3. Compilar y probar

1. Abre una terminal en la raíz del proyecto.
2. Compila los binarios base:
   ```bash
   make
   ```
3. Ejecuta las pruebas unitarias:
   ```bash
   make test
   ```
4. (Opcional) Compila con eBPF y asigna capacidades solo a `bin/servicio_b`:
   ```bash
   make ebpf
   sudo setcap cap_bpf,cap_perfmon=ep bin/servicio_b
   ```
5. Limpia los binarios compilados cuando sea necesario:
   ```bash
   make clean
   ```

---

## 4. Ejecutar TraceLite

### Observación de procesos del sistema

- Captura instantánea por PID o nombre:
  ```bash
  ./bin/tracelite [PID | NOMBRE]
  ```
- Captura consolidada con `--resumen`:
  ```bash
  ./bin/tracelite --resumen -d 30 --interval 150 antigravity
  ```

### Solicitudes con evaluación de anomalías

- **Programa nuevo:**
  ```bash
  ./bin/tracelite --resumen -- gzip -kfc datos/normal.bin
  ```
- **Backend HTTP (`127.0.0.1`):**
  ```bash
  ./bin/tracelite --resumen --modo respuesta --backend 127.0.0.1:8000 --ruta "/hash?mb=8"
  ```
- **Aplicación del sistema:**
  ```bash
  ./bin/tracelite --resumen --modo respuesta --app firefox -- firefox --new-tab https://example.com
  ```
- **Opciones principales:**
  - `--modo`: `respuesta`, `estabilidad`, `escalado`, `fallo`.
  - `--tiempo`: `normal`, `alto`, `muy-alto`, o `--plazo SEG`.
  - `--fuente`: `auto`, `ebpf`, `proc`.
- **Historial JSONL:**
  ```bash
  ./bin/tracelite --historial
  ./bin/tracelite --historial --trace <trace_id>
  ```

---

## 5. Scripts de `pruebas/`

Ejecuta los scripts desde la raíz del proyecto para validar servicios reales y eBPF:

```bash
./pruebas/prueba_base.sh
./pruebas/diagnostico_ebpf.sh
./pruebas/demo_servicios_so.sh
./pruebas/pruebas_vivos.sh
```

---

## 6. Limitaciones conocidas

- La captura eBPF por conexión soporta IPv4 en `127.0.0.1` sobre HTTP/1.0 sin TLS.
- El respaldo `/proc` muestrea cada 20 ms y puede omitir procesos más cortos.
- eBPF requiere kernel Linux con BTF habilitado (`/sys/kernel/btf/vmlinux`).
