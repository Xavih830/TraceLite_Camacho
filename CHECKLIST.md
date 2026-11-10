- [x] F0 Linux verificado, esqueleto y Makefile compilan
- [x] F1 span/utilidades + `make test` verde
- [x] F2 colector imprime árbol con datos fabricados
- [x] F3 api e inventario propagan trace_id/parent
- [x] F4 launcher: 6 trazas / 11 spans, exit 0
- [ ] F5 sin zombies, pstree, kill -9, time, valgrind
- [ ] F6 README, diagrama, capturas, matriz revisada

## Notas de cumplimiento de la Sección 0.1 (Límites de conocimiento)
- **`_Static_assert` y `PIPE_BUF`:** En lugar de `_Static_assert(sizeof(span_t) <= PIPE_BUF, ...)` en tiempo de compilación, se usa `assert(sizeof(span_t) <= 512)` en tiempo de ejecución (`sizeof(span_t) == 264` bytes), garantizando atomicidad de escritura en pipes POSIX sin salir de lo visto en clase.
- **`/dev/urandom` y `rand_r`:** En lugar de leer `/dev/urandom` o usar `rand_r`, se utiliza `rand()` inicializando la semilla una vez por proceso con `srand(getpid() ^ time(NULL))`.
- **`signal(SIGPIPE, SIG_IGN)`:** No se utiliza manejo de señales explícito; se controla el ciclo de vida y cierre ordenado de descriptores (cascada de EOF) y el retorno de `write`/`read`.
- **`valgrind` y `getrusage`:** No se utilizan, respetando la lista de confirmación de la sección 0.1.
