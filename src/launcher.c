#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Verificación de descriptores por proceso (Sección 4 y Regla 5 de Sección 14):
 *
 * Pipes creados por launcher antes de cualquier fork (8 descriptores en total):
 *   preq[0], preq[1] : launcher -> api        (req_t)
 *   pin[0],  pin[1]  : api -> inventario      (ctx_t)
 *   pout[0], pout[1] : inventario -> api      (resp_t)
 *   psp[0],  psp[1]  : api, inventario -> collector (span_t)
 *
 * Descriptores que CADA proceso conserva (todo lo demás se cierra antes de execv):
 *   - collector  : conserva únicamente { psp[0] }
 *                  cierra preq[0], preq[1], pin[0], pin[1], pout[0], pout[1], psp[1]
 *   - inventario : conserva únicamente { pin[0], pout[1], psp[1] }
 *                  cierra preq[0], preq[1], pin[1], pout[0], psp[0]
 *   - api        : conserva únicamente { preq[0], pin[1], pout[0], psp[1] }
 *                  cierra preq[1], pin[0], pout[1], psp[0]
 *   - launcher   : tras los 3 forks conserva únicamente { preq[1] }
 *                  cierra preq[0], pin[0], pin[1], pout[0], pout[1], psp[0], psp[1]
 *                  y al terminar de enviar las 6 peticiones cierra preq[1] (inicio de cascada EOF).
 */

static int esta_en_lista(int fd, const int *lista, int n) {
    for (int i = 0; i < n; i++) {
        if (lista[i] == fd) {
            return 1;
        }
    }
    return 0;
}

static pid_t lanzar(const char *ruta_bin, const int *conservar, int n_conservar,
                    const int *todos, int n_todos) {
    /* Vaciar buffers estándar antes de fork para evitar salida duplicada */
    fflush(stdout);
    fflush(stderr);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }

    if (pid == 0) {
        /* Hijo: cierre selectivo de todos los fds que no le corresponden */
        for (int i = 0; i < n_todos; i++) {
            if (!esta_en_lista(todos[i], conservar, n_conservar)) {
                close(todos[i]);
            }
        }

        char args_fd[4][16];
        char *argv_exec[6];
        argv_exec[0] = (char *)ruta_bin;
        for (int i = 0; i < n_conservar; i++) {
            snprintf(args_fd[i], sizeof(args_fd[i]), "%d", conservar[i]);
            argv_exec[i + 1] = args_fd[i];
        }
        argv_exec[n_conservar + 1] = NULL;

        execv(ruta_bin, argv_exec);
        perror("execv");
        _exit(127);
    }

    return pid;
}

int main(int argc, char *argv[]) {
    int modo_lento = (argc > 1 && strcmp(argv[1], "--lento") == 0);

    int preq[2], pin[2], pout[2], psp[2];
    if (pipe(preq) < 0 || pipe(pin) < 0 || pipe(pout) < 0 || pipe(psp) < 0) {
        perror("pipe");
        return 1;
    }

    int todos[8] = {
        preq[0], preq[1],
        pin[0],  pin[1],
        pout[0], pout[1],
        psp[0],  psp[1]
    };

    /* Conjuntos de descriptores que conserva cada hijo según la sección 4 */
    int fds_collector[1]  = { psp[0] };
    int fds_inventario[3] = { pin[0], pout[1], psp[1] };
    int fds_api[4]        = { preq[0], pin[1], pout[0], psp[1] };

    /* Lanzar los 3 servicios en el orden: collector, inventario, api */
    pid_t pid_col = lanzar("./bin/collector",  fds_collector,  1, todos, 8);
    pid_t pid_inv = lanzar("./bin/inventario", fds_inventario, 3, todos, 8);
    pid_t pid_api = lanzar("./bin/api",        fds_api,        4, todos, 8);

    if (pid_col < 0 || pid_inv < 0 || pid_api < 0) {
        return 1;
    }

    /* En el padre (launcher), cerrar todos los descriptores salvo preq[1] */
    close(preq[0]);
    close(pin[0]);
    close(pin[1]);
    close(pout[0]);
    close(pout[1]);
    close(psp[0]);
    close(psp[1]);

    /* 6 peticiones de prueba según la tabla de la sección 3.4 */
    req_t peticiones[6] = {
        { 1, "GET /products", 0,   1, 0 },
        { 2, "GET /health",   0,   0, 0 },
        { 3, "POST /buy",     1,   1, 0 },
        { 4, "POST /buy",     2,   1, 1 },
        { 5, "POST /buy",     3,   1, 2 },
        { 6, "POST /buy",     4, 999, 0 }
    };

    for (int i = 0; i < 6; i++) {
        if (escribir_exacto(preq[1], &peticiones[i], sizeof(req_t)) != 1) {
            fprintf(stderr, "[launcher] error al escribir petición %d\n", peticiones[i].id);
            break;
        }
        if (modo_lento) {
            dormir_ms(600);
        }
    }

    /* Cerrar preq[1] para iniciar la cascada de EOF */
    close(preq[1]);

    /* Esperar a los tres hijos con waitpid para evitar procesos zombie */
    pid_t pids[3] = { pid_col, pid_inv, pid_api };
    const char *nombres[3] = { "collector", "inventario", "api" };
    int estados[3] = { 0, 0, 0 };

    for (int i = 0; i < 3; i++) {
        if (waitpid(pids[i], &estados[i], 0) < 0) {
            perror("waitpid");
            estados[i] = -1;
        }
    }

    int codigo_retorno = 0;
    for (int i = 0; i < 3; i++) {
        if (WIFEXITED(estados[i])) {
            int cod = WEXITSTATUS(estados[i]);
            printf("[launcher] %s terminó con código %d\n", nombres[i], cod);
            if (cod != 0) {
                codigo_retorno = 1;
            }
        } else if (WIFSIGNALED(estados[i])) {
            int sig = WTERMSIG(estados[i]);
            printf("[launcher] %s terminó por señal %d\n", nombres[i], sig);
            codigo_retorno = 1;
        } else {
            printf("[launcher] %s terminó de forma anormal\n", nombres[i]);
            codigo_retorno = 1;
        }
    }

    return codigo_retorno;
}
