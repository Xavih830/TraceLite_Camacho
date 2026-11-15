# Plan de pruebas y capturas de TraceLite en Lubuntu

Este plan lleva paso a paso cada prueba de la sección 6 del informe, indica qué captura tomar y qué valores anotar para llenar los marcadores `[[ ]]` del documento. Todo se ejecuta en la partición Lubuntu, con la versión actual del proyecto, que ya no tiene el modo `--watch`. Por eso no hay prueba 6.7, y la evidencia de uso de CPU de la prueba 6.4 sale de un comando externo (`ps` o `top`), no de la columna `OPERACION`.

## 0. Preparación general

Antes de empezar conviene dejar todo en un estado conocido y fijar cómo se toman las capturas.

```bash
cd ~/ruta/del/proyecto            # raíz del proyecto, donde está el Makefile
mkdir -p capturas                 # aquí se guardan todas las imágenes
ls docs/pruebas/                  # deben existir tsp_np.py, carga_np.sh y servicio_efimero.sh
python3 docs/pruebas/tsp_np.py 6  # prueba rápida de que el guion corre (ajusta el argumento si el guion pide otros)
pgrep -fl antigravity | head -5   # Antigravity debe estar abierto y reconocible por nombre
ps -ef | grep -E "collector|monitor_pid|tracelite" | grep -v grep   # no debe haber restos de corridas anteriores
ls /tmp/tracelite.sock 2>/dev/null   # si existe y no hay collector vivo, es un resto y se borra con rm
```

Si `docs/pruebas/` no tiene los tres guiones, envíamelos o dime que los escriba antes de seguir, porque las pruebas 6.4, 6.5 y 6.6 dependen de ellos.

Para las capturas se recomienda lo siguiente.

1. Maximizar la terminal y usar una fuente de 12 a 14 puntos. El colector adapta la tabla al ancho con `TIOCGWINSZ`, así que una terminal angosta rompe las filas.
2. No redirigir la salida con `| tee` ni `> archivo` cuando la captura sea de la tabla, porque el colector pierde el ancho y los colores al dejar de ser una terminal.
3. Tomar la captura con la tecla Impr Pant (o `scrot -s` para elegir el área) y guardarla en `capturas/` con el nombre exacto indicado en cada prueba.
4. Recortar solo la parte útil de la pantalla. Los colores de `ESTADO` y `ANOMALIA` deben verse.
5. Después de cada prueba, anotar los valores pedidos en la hoja de la sección 8, mientras la pantalla todavía está abierta.

## 1. Prueba C0, entorno de pruebas (sección 6.1, Tabla 8)

Esta captura llena la Tabla 8 completa. Ejecuta todo en una sola terminal para que quepa en una imagen.

```bash
uname -a; lsb_release -d; nproc
gcc --version | head -1; python3 --version
grep -n CFLAGS Makefile
make clean && make 2>&1 | tee /tmp/make.log; echo "advertencias: $(grep -ci warning /tmp/make.log)"
```

Captura: `capturas/C0_entorno_compilacion.png`.

Cada valor de la Tabla 8 sale de una línea concreta.

| Fila de la Tabla 8 | De dónde sale |
| --- | --- |
| Distribución | Línea `Description` de `lsb_release -d` (por ejemplo, "Lubuntu 24.04 LTS") | -> Ubuntu 24.04.5 LTS
| Kernel | Versión que muestra `uname -a` (el campo parecido a `6.8.0-xx-generic`) | -> 7.0.0-43-generic
| Núcleos de CPU | Número que imprime `nproc` | -> 16
| Compilador | Primera línea de `gcc --version` | -> gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
| Intérprete de la carga | Salida de `python3 --version` | -> Python 3.12.3
| Compilación | Si `make` terminó sin errores y la línea `advertencias: 0`, junto con los flags de `CFLAGS` (el informe afirma `-Wall -Wextra`) | -> Sí usa

Si `CFLAGS` no incluye `-Wall -Wextra` o hay advertencias, avísame, porque cambia lo que dice la sección 6.1.

## 2. Prueba C1, prueba unitaria del núcleo común (sección 6.2)

```bash
make test
```

Captura: `capturas/C1_make_test.png`. Debe verse la línea `[OK] tests/test_span superado con exito`, el valor de `sizeof(span_t)` (se espera 264) y la duración medida tras la espera de 20 ms (en WSL dio 20,19 ms, aquí puede variar). Si alguna comprobación falla, no avances a las demás pruebas.

## 3. Prueba 6.3, solicitud 1, Antigravity en reposo

Condición previa. Antigravity debe estar abierto, con el agente sin ninguna tarea en ejecución. Espera un minuto para que el IDE termine de cargar.

### 3.1 Traza completa

```bash
./bin/tracelite antigravity
```

Captura: `capturas/P1_traza_reposo.png`. Debe verse la resolución del nombre al PID raíz, la lista del árbol, la tabla de spans y la línea `-- Resumen --`. Si la tabla es más larga que la pantalla, reduce la fuente o toma dos capturas (inicio y final) y dime cuáles son.

Anota el PID raíz, el número de procesos k del árbol, el número de spans (debería ser k más 1), y los errores y anomalías del `-- Resumen --`.

### 3.2 Evidencia de que los procesos están en espera

Como la columna `OPERACION` ya no muestra el estado, esta evidencia sale de un comando externo. Ejecútalo justo después de la traza.

```bash
ps -o pid,stat,%cpu,wchan:20,cmd -p $(pgrep -d, -f antigravity | cut -d, -f1-12)
```

Captura: `capturas/P1_ps_reposo.png`. Se espera ver `STAT` en `S` y `%CPU` bajo, con `wchan` como `ep_poll` o `futex_wait_queue`, que es lo que corresponde a un IDE sin tareas. Es un complemento nuevo respecto al informe actual, y lo incluyo en la versión final.

### 3.3 Modelo de procesos y ausencia de zombis

```bash
./bin/tracelite antigravity > /dev/null & sleep 0.1; \
  ps -ef --forest | grep -E "tracelite|collector|monitor_pid" | grep -v grep; wait
echo "defunct restantes: $(ps -ef | grep -c '[d]efunct')"
```

Captura: `capturas/P1_procesos_forest.png`. Debe verse a `tracelite` como padre de `collector` y de varios `monitor_pid`, y al final `defunct restantes: 0`. Si no aparecen los `monitor_pid`, cambia `sleep 0.1` por `sleep 0.05` o `sleep 0.15` hasta que se vean, porque su ventana es de 250 ms.

## 4. Prueba 6.4, solicitud 2, Antigravity bajo carga de cómputo intensivo

Necesitas dos terminales. En la terminal del agente de Antigravity pide lo siguiente, sin que lo optimice ni lo interrumpa.

> Ejecuta en la terminal `cd docs/pruebas && python3 tsp_np.py 12` y muéstrame la salida.

Con 12 ciudades el cálculo tarda del orden de un minuto, aunque ese dato es una referencia de otra máquina y debes anotar tu propio tiempo. Mientras corre, en la otra terminal ejecuta lo siguiente de una sola vez.

```bash
PID=$(pgrep -f '^python3 tsp_np.py 12$' | head -1); echo "PID de python3: $PID"
ps -o pid,stat,%cpu,etime,cmd -p $PID
top -b -n 2 -d 1 -p $PID | tail -2
pstree -p -s $PID
./bin/tracelite antigravity
```

El patrón con `^` evita que `pgrep` devuelva el `bash` del agente, cuya línea de comandos también contiene el texto `tsp_np.py 12`. Capturas.

| Archivo | Contenido |
| --- | --- |
| `capturas/P2_ps_top.png` | Salida de `ps` y `top`, con `STAT` en `R` y `%CPU` cercano a 100 |
| `capturas/P2_traza_carga_np.png` | Tabla de `tracelite` con el span `python3 tsp_np.py 12` |
| `capturas/P2_pstree_python.png` | `pstree` que muestra la cadena desde Antigravity hasta `python3` (opcional) |

Anota el PID de `python3`, el número de spans, los errores y las anomalías, y qué proceso aparece como su padre. Si `python3` no aparece en la tabla por el límite de 32 nodos, observa desde su `bash`.

```bash
PID_BASH=$(ps -o ppid= -p $PID | tr -d ' ')
./bin/tracelite $PID_BASH
```

En ese caso, dime que lo hiciste así para dejarlo escrito en el informe.

Esta captura reemplaza la evidencia del `[CPU]` en `OPERACION`. El span de `python3` muestra la línea de comandos que tenía al iniciar la observación, y el uso de CPU lo demuestra el `ps`/`top` tomado en el mismo momento y correlacionado por PID.

## 5. Prueba 6.5, solicitud 3, incremento aleatorio del tiempo de respuesta

Esta es la prueba central de la anomalía de tiempo. Abre dos terminales o usa la del agente y una aparte.

1. En la terminal propia inicia la observación.

```bash
./bin/tracelite --resumen -d 60 antigravity
```

2. De inmediato, en el chat del agente de Antigravity pide lo siguiente.

> Ejecuta en la terminal `bash docs/pruebas/carga_np.sh` y muéstrame la salida.

El guion corre cuatro solicitudes TSP, de las cuales tres usan 10 ciudades y una, elegida al azar, usa 11. Todo el guion dura del orden de diez segundos, y `tracelite` sigue muestreando hasta completar los 60 s. Con `Ctrl+C` la observación termina de forma ordenada y se imprime igual la tabla final.

Variante de respaldo, si el agente no se deja controlar. Ejecuta el guion directamente en la terminal integrada.

```bash
bash docs/pruebas/carga_np.sh & ./bin/tracelite --resumen -d 0 $!
```

Capturas.

| Archivo | Contenido |
| --- | --- |
| `capturas/P3_salida_carga.png` | Salida de `carga_np.sh`, con la solicitud pesada y los tiempos de `tsp_np.py` |
| `capturas/P3_resumen_latencia_alta.png` | Tabla final con el span amarillo `latencia_alta` y su `mu` y `umbral` visibles |

Si la columna `msg` queda cortada, amplía la ventana o toma la captura de la fila por separado. Lo importante es que se lean el span marcado, su duración y el texto `mu=..ms umbral=..ms`.

Anota los valores siguientes, porque con ellos el informe comprueba las tres condiciones.

1. La solicitud elegida al azar, que es la pesada (el número i).
2. Las tres duraciones livianas d1, d2 y d3 y la duración pesada di, tal como salen del span.
3. La media μ y el umbral U que aparecen en el `msg`.
4. Los tiempos que imprimió `tsp_np.py` para cada solicitud.
5. Si algún otro proceso, como `bash` o un proceso de Antigravity, también salió con `latencia_alta`.
6. El número de spans, errores y anomalías del `-- Resumen --`.

Si ninguna solicitud sale marcada, repite la prueba, porque la solicitud pesada cambia en cada ejecución.

## 6. Prueba 6.6, escenario de error por terminación abrupta

En la terminal del agente pide otra vez la carga de 12 ciudades.

> Ejecuta en la terminal `cd docs/pruebas && python3 tsp_np.py 12` y muéstrame la salida.

Mientras corre, en la otra terminal ejecuta lo siguiente.

```bash
./bin/tracelite antigravity & sleep 0.1; pkill -9 -f '^python3 tsp_np.py 12'; wait
```

Captura: `capturas/P4_error_proceso.png`. Debe verse el span de `python3` con `ERROR`, el texto `proceso_termino`, la etiqueta `[error_proceso]` y la línea final con `1 errores`. La columna `OPERACION` conserva `python3 tsp_np.py 12`, ya que no se le agrega ninguna marca.

El instante del `SIGKILL` importa, porque el monitor lee `/proc` al inicio y al final de su ventana de 250 ms.

1. Si el span sale con `no_existe_en_proc`, el proceso murió antes de que el monitor empezara. Aumenta el `sleep` a 0.15 o 0.2.
2. Si `python3` no aparece en la traza, el `SIGKILL` llegó antes del escaneo. Haz lo mismo.
3. Si el span sale `OK`, el `SIGKILL` llegó después de la ventana. Reduce el `sleep` a 0.05.

Variante de respaldo, si no se logra el instante con `python3`.

```bash
bash docs/pruebas/servicio_efimero.sh & ./bin/tracelite $!; kill %1
```

Con esta variante el span con error es el de `sleep 0.2`, y debes decírmelo para ajustar el texto.

## 7. Qué hacer entre pruebas

Antes de cada prueba comprueba que no haya restos de la anterior.

```bash
ps -ef | grep -E "collector|monitor_pid|tsp_np" | grep -v grep
```

Si queda algo vivo después de una interrupción, termínalo con `pkill -f "collector|monitor_pid|tsp_np"` y vuelve a verificar.

## 8. Hoja de valores a enviarme

Con las capturas y esta hoja lleno todos los marcadores del informe y la Tabla 9.

| Prueba | Valores que debes anotar |
| --- | --- |
| C0 | Distribución, kernel, núcleos, versión de `gcc`, versión de Python, flags de `CFLAGS`, número de advertencias |
| C1 | `sizeof(span_t)`, duración medida de la espera de 20 ms, estado de todas las comprobaciones |
| 6.3 | PID raíz, k, número de spans, errores, anomalías, y qué muestran `STAT`, `%CPU` y `wchan` en reposo |
| 6.4 | PID de `python3`, tiempo que tardó `tsp_np.py 12`, número de spans, errores, anomalías, padre de `python3`, `%CPU` y `STAT` |
| 6.5 | i, d1, d2, d3, di, μ, U, tiempos de `tsp_np.py`, spans, errores, anomalías, otros spans marcados |
| 6.6 | Valor de `sleep` que funcionó, mensaje del span (`proceso_termino` u otro), número de errores y de spans |

## 9. Lista de capturas

| Archivo | Prueba |
| --- | --- |
| `C0_entorno_compilacion.png` | 6.1 |
| `C1_make_test.png` | 6.2 |
| `P1_traza_reposo.png` | 6.3 |
| `P1_ps_reposo.png` | 6.3 (nueva, evidencia de espera) |
| `P1_procesos_forest.png` | 6.3 |
| `P2_ps_top.png` | 6.4 (nueva, evidencia de CPU) |
| `P2_traza_carga_np.png` | 6.4 |
| `P2_pstree_python.png` | 6.4 (opcional) |
| `P3_salida_carga.png` | 6.5 |
| `P3_resumen_latencia_alta.png` | 6.5 |
| `P4_error_proceso.png` | 6.6 |

Las capturas marcadas como nuevas no están en el informe actual y las incorporo al reconstruirlo, junto con la eliminación de la prueba 6.7 y de lo relacionado con `--watch`.
