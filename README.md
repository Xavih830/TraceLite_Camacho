# TraceLite

TraceLite observa árboles reales de procesos en GNU/Linux. El sistema usa `/proc`, `fork()`, `execv()`, Sockets UNIX (`AF_UNIX`) e hilos (`pthread`).

---

## 1. Estructura del proyecto

- `Makefile`: automatiza la compilación y las pruebas.
- `README.md`: describe la compilación y el uso.
- `include/span.h`: define la estructura `span_t` (264 bytes).
- `include/common.h`: declara utilidades comunes de tiempo, IDs y E/S.
- `src/common.c`: implementa las utilidades comunes.
- `src/tracelite.c`: descubre procesos en `/proc` y coordina la captura.
- `src/monitor_pid.c`: observa un PID en `/proc` y envía su span al colector.
- `src/collector.c`: recibe spans por Socket UNIX, detecta anomalías e imprime la tabla.
- `tests/test_span.c`: verifica `span_t` y la lectura/escritura exacta.
- `pruebas/`: contiene scripts de carga y anomalías (`carga_np.sh`, `servicio_efimero.sh`, `tsp_np.py`).

---

## 2. Requisitos

- Sistema operativo GNU/Linux (nativo o WSL2).
- Compilador `gcc` con soporte `pthread` y `libm`.
- Herramienta `make`.

---

## 3. Compilar y probar

1. Abre una terminal en la raíz del proyecto.
2. Compila los binarios:
   ```bash
   make
   ```
3. Ejecuta las pruebas unitarias:
   ```bash
   make test
   ```
4. Limpia los binarios generados cuando sea necesario:
   ```bash
   make clean
   ```

---

## 4. Ejecutar TraceLite

### Modo instantánea (predeterminado)

Captura el subárbol actual de un proceso. Lanza un proceso `monitor_pid` en paralelo por cada nodo mediante `fork()` y `execv()`.

- Selecciona un árbol automáticamente:
  ```bash
  ./bin/tracelite
  ```
- Observa un proceso por su PID:
  ```bash
  ./bin/tracelite <PID>
  ```
- Observa un proceso por su nombre:
  ```bash
  ./bin/tracelite antigravity
  ./bin/tracelite chrome
  ./bin/tracelite postgres
  ```
- Muestra la ayuda y ejemplos:
  ```bash
  ./bin/tracelite --help
  ```

### Modo resumen (`--resumen`)

Monitorea el subárbol durante una ventana continua. Al finalizar, consolida cada proceso en una tabla jerárquica única.

- Captura durante 60 segundos (valor predeterminado):
  ```bash
  ./bin/tracelite --resumen antigravity
  ```
- Define una duración en segundos con `-d` y un intervalo en ms con `--interval`:
  ```bash
  ./bin/tracelite --resumen -d 30 --interval 150 antigravity
  ./bin/tracelite --resumen -d 20 chrome
  ```
- Usa `-d 0` para detener la captura manualmente con `Ctrl+C`.

---

## 5. Detección de anomalías y pruebas

El colector clasifica dos tipos de anomalía en la columna `ESTADO`:

- `ERROR [error_proceso]`: ocurre cuando un PID no existe o termina durante la captura.
  ```bash
  ./bin/tracelite 999999
  ```
- `ANOMALIA [latencia_alta]`: ocurre cuando la duración de un span supera $\mu_{-i} + 2\sigma_{-i}$ (con margen mínimo de $1.5\,\mu_{-i}$ y $+10\text{ ms}$) frente a sus pares.

Para probar carga real y anomalías en Linux, ejecuta los scripts de `pruebas/`:

```bash
./pruebas/carga_np.sh
./pruebas/servicio_efimero.sh
```
