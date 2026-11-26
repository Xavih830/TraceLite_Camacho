# Guía de Pruebas de TraceLite con Antigravity (`antigravity-ide`)

Este documento reúne los comandos de prueba centrados en **`antigravity-ide`** como aplicación viva del sistema operativo, su justificación técnica frente a la evaluación del proyecto y la explicación detallada de cada argumento utilizado.

---

## 1. Requisitos previos de compilación y permisos

- **Compilación estándar (`/proc` + GUI):**
  ```bash
  make && make gui
  ```
- **Compilación con eBPF (opcional, para captura en kernel sin depender solo del muestreo `/proc`):**
  ```bash
  make ebpf && sudo setcap cap_bpf,cap_perfmon=ep bin/servicio_b
  ```
  > Nota: Si ejecutas `make` después de `make ebpf`, `bin/servicio_b` se recompila sin eBPF; vuelve a ejecutar `make ebpf && sudo setcap ...` si deseas reactivarlo.

---

## 2. Explicación de argumentos

| Argumento | Qué hace |
|---|---|
| `--resumen` | Ejecuta un lote de solicitudes (por defecto `5`, o según `--entradas`) y consolida las trazas y el resumen final. En modo observación pura activa el muestreo continuo. |
| `-d <seg>` | Duración en segundos de la ventana de observación por solicitud (en `--app`) o de la captura continua (en observación pura). Permite comparar solicitudes rápidas junto con el árbol completo de `antigravity-ide` en esa ventana. |
| `--interval <ms>` | Intervalo de muestreo en milisegundos sobre `/proc` en modo observación pura (por defecto `150 ms`). |
| `--app <nombre\|PID>` | Apunta a una aplicación viva del SO (`antigravity-ide`) sin cerrarla al finalizar. `servicio_b` muestrea su árbol de procesos real durante la solicitud. |
| `--fin <orden\|inactividad>` | Política de cierre de la observación en `--app`: `orden` cierra la ventana al terminar la orden + `-d`; `inactividad` espera a que el consumo de CPU baje a la línea base. |
| `--linea-base <seg>` | Segundos previos de calibración de CPU sobre la app viva (`0` omite esa espera inicial cuando la app ya tiene carga variable como un IDE). |
| `--modo <modo>` | Criterio de evaluación: `respuesta` (latencia/SLO), `estabilidad` ($\mu + 2\sigma$), `escalado` (`dur/tamano`) o `fallo` (inyección controlada de error). |
| `--slo-ms <ms>` | Límite máximo esperado en milisegundos para `modo=respuesta`; si se supera, emite alerta `slo_excedido`. |
| `--plazo <seg>` | Timeout manual máximo por solicitud en segundos (reemplaza en la GUI a los antiguos presets de tiempo). |
| `--entradas "<e1,e2,...>"` | Lista separada por comas de entradas; determina cuántas solicitudes se lanzan (ej. 5 valores = 5 solicitudes). |
| `--tamanos "<t1,t2,...>"` | Pesos o tamaños relativos asociados a cada entrada en `--modo escalado` para detectar crecimiento no lineal (`escalado_no_lineal`). |
| `--fallo-en <N>` | En `--modo fallo`, fuerza un fallo en la solicitud número `N` (`ERROR [error_proceso]`) para validar resiliencia sin afectar las demás ni a la app viva. |
| `-- <comando>` | Separa las opciones de TraceLite de la orden real que ejecuta `servicio_a` (por ejemplo `./docs/pruebas/tsp_np.py` o `sleep`). |
| `--historial` | Consulta las trazas persistidas en disco (`~/.local/share/tracelite/YYYY-MM-DD.jsonl`) sin ejecutar nuevas solicitudes. |

---

## 3. Escenarios de prueba centrados en `antigravity-ide`

### Escenario A — Observación continua del árbol real de procesos (sin orden cliente)
```bash
./bin/tracelite --resumen -d 10 --interval 150 antigravity-ide
```
- **Qué prueba:** Descubrimiento recursivo por `PPID` en `/proc` del árbol real de procesos de `antigravity-ide` (proceso principal, `chrome-sandbox`, `bash`, `agy`, sondeos `cpuUsage.sh`/`ps`) y barras escalonadas según nacimiento y término de cada hijo.
- **Justificación:** Demuestra observabilidad real sobre un proceso complejo vivo del SO sin invadirlo ni depender de cargas sintéticas.

---

### Escenario B — Modo `respuesta` + `--slo-ms` sobre `antigravity-ide`
```bash
./bin/tracelite --resumen -d 10 --app antigravity-ide --fin orden --linea-base 0 --modo respuesta --slo-ms 10500 --entradas "9,9,10,9,9" -- ./docs/pruebas/tsp_np.py
```
- **Qué prueba:** 5 solicitudes correlacionadas (`servicio_a` + `servicio_b` + árbol vivo de `antigravity-ide` por cada una), verificando cumplimiento del SLO fijado (`10500 ms` considerando la ventana `-d 10`).
- **Justificación:** Valida si la operación junto con su ventana de observación cumple el umbral fijo de tiempo acordado; si se reduce `--slo-ms` (por ejemplo `9500`), dispara la alerta `slo_excedido`.

---

### Escenario C — Modo `estabilidad` sobre `antigravity-ide`
```bash
./bin/tracelite --resumen -d 10 --app antigravity-ide --fin orden --linea-base 0 --modo estabilidad --entradas "10,10,10,10,10" -- ./docs/pruebas/tsp_np.py
```
- **Qué prueba:** 5 solicitudes idénticas (`n=10` en `tsp_np.py`) sobre `antigravity-ide` para calcular media ($\mu$) y desviación estándar ($\sigma$), detectando `latencia_alta` ($\mu + 2\sigma$) si ocurre contención real en el sistema (compilación en paralelo, carga en el IDE, etc.).
- **Justificación:** Permite detectar degradación intermitente en ejecuciones repetibles sin requerir un umbral fijo previo.

---

### Escenario D — Modo `escalado` sobre `antigravity-ide`
```bash
./bin/tracelite --resumen -d 10 --app antigravity-ide --fin orden --linea-base 0 --modo escalado --entradas "7,8,9,10,11" --tamanos "1,2,4,8,16" -- ./docs/pruebas/tsp_np.py
```
- **Qué prueba:** 5 solicitudes con complejidad creciente (`n = 7..11`) en un algoritmo NP-completo (`tsp_np.py`) correlacionado con `antigravity-ide`, disparando `ANOMALIA [escalado_no_lineal]` cuando la razón `duración / tamaño` supera $1.5 \times \text{mediana}$.
- **Justificación:** Demuestra detección de cuellos de botella algorítmicos y crecimiento no lineal frente a entradas crecientes.

---

### Escenario E — Modo `fallo` sobre `antigravity-ide`
```bash
./bin/tracelite --resumen -d 10 --app antigravity-ide --fin orden --linea-base 0 --modo fallo --fallo-en 3 --entradas "0.2,0.2,0.2,0.2,0.2" -- sleep
```
- **Qué prueba:** 5 solicitudes sobre la app viva inyectando un fallo controlado en la solicitud 3 (`ERROR [error_proceso]` y `[ALERTA]`), verificando que las solicitudes 1, 2, 4 y 5 continúan normalmente y que `antigravity-ide` conserva su PID intacto.
- **Justificación:** Valida resiliencia del orquestador, registro de errores y aislamiento entre la herramienta de trazado y la aplicación observada.

---

### Escenario F — Persistencia, interfaz gráfica y suites automáticas
- **Consultar persistencia en consola:**
  ```bash
  ./bin/tracelite --historial
  ```
- **Abrir visor y lanzador gráfico GTK 4 + Cairo:**
  ```bash
  ./bin/tracelite-gui
  ```
- **Ejecutar suites automáticas completas:**
  ```bash
  make test
  make check
  bash docs/pruebas/pruebas_vivos.sh
  ```

---

## 4. Referencias

- **Código fuente del proyecto:**
  - Orquestación CLI y modo observación: `src/tracelite.c`
  - Gestión de lotes, modos y alertas: `src/solicitudes.c`, `include/solicitudes.h`
  - Emisor y observador IPC: `src/servicio_a.c`, `src/servicio_b.c`, `include/protocolo.h`
  - Tabla jerárquica y columna `RELACION`: `src/tabla.c`, `include/tabla.h`
  - Persistencia JSONL diaria: `src/almacen.c`, `include/almacen.h`
  - Interfaz gráfica GTK 4 + Cairo: `src/gui.c`, `src/gui_form.c`, `include/gui_form.h`
  - Programa de carga NP-completo y suite viva: `docs/pruebas/tsp_np.py`, `docs/pruebas/pruebas_vivos.sh`
- **Documentación interna:** `README.md`, `CHECKLIST.md`, `docs/INFORME_FINAL_TRACELITE.md`, `docs/Updates.md`
- **Estándares y APIs del SO:**
  - Linux `proc(5)` (`/proc/<pid>/stat`, `/proc/<pid>/cmdline`) y `capabilities(7)` (`cap_bpf`, `cap_perfmon`)
  - W3C Trace Context (`traceparent`): https://www.w3.org/TR/trace-context/
  - OpenTelemetry Span Model: https://opentelemetry.io/docs/specs/otel/trace/api/
