#include "common.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Opción 2: Trazado de comandos/programas existentes del SO ("Wrapper").
 * Selecciona una lista de comandos reales del SO (por argv o predeterminados),
 * lanza cada uno con fork() + execvp(), mide su tiempo real con waitpid()
 * y envía los spans al colector mediante un Socket UNIX (AF_UNIX).
 */

static int conectar_colector(const char *sock_path) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);

    for (int intento = 0; intento < 20; intento++) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            return -1;
        }
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            return fd;
        }
        close(fd);
        dormir_ms(25);
    }
    return -1;
}

static void ejecutar_y_trazar_comando(int sock_fd, const char *trace_id,
                                      const char *parent_id, char *const argv_cmd[]) {
    char op_desc[48];
    if (argv_cmd[1] != NULL) {
        snprintf(op_desc, sizeof(op_desc), "%s %s", argv_cmd[0], argv_cmd[1]);
    } else {
        snprintf(op_desc, sizeof(op_desc), "%s", argv_cmd[0]);
    }

    span_t s;
    span_ini(&s, trace_id, parent_id, "os_cmd", op_desc);

    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        span_fin(&s, 1, "fork_fallo", "");
        escribir_exacto(sock_fd, &s, sizeof(span_t));
        return;
    }

    if (pid == 0) {
        /* Redirigir stdout y stderr del comando a /dev/null para mantener limpia la salida de trazas */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execvp(argv_cmd[0], argv_cmd);
        _exit(127);
    }

    int estado_hijo = 0;
    waitpid(pid, &estado_hijo, 0);

    /* Registrar el PID real del comando del SO que ejecutó el trabajo */
    s.pid = (int)pid;

    if (WIFEXITED(estado_hijo) && WEXITSTATUS(estado_hijo) == 0) {
        span_fin(&s, 0, "", "");
    } else if (WIFEXITED(estado_hijo)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "exit_%d", WEXITSTATUS(estado_hijo));
        span_fin(&s, 1, msg, "");
    } else {
        span_fin(&s, 1, "senal", "");
    }

    escribir_exacto(sock_fd, &s, sizeof(span_t));
}

int main(int argc, char *argv[]) {
    const char *sock_path = "/tmp/tracelite_op2.sock";

    /* 1. Lanzar collector_sock con fork() + execv() */
    fflush(stdout);
    pid_t pid_col = fork();
    if (pid_col < 0) {
        perror("fork collector");
        return 1;
    }
    if (pid_col == 0) {
        char *args_col[3] = { "./collector_sock", (char *)sock_path, NULL };
        execv("./collector_sock", args_col);
        perror("execv collector_sock");
        _exit(127);
    }

    int sock_fd = conectar_colector(sock_path);
    if (sock_fd < 0) {
        fprintf(stderr, "[wrapper] no se pudo conectar a %s\n", sock_path);
        waitpid(pid_col, NULL, 0);
        return 1;
    }

    /* 2. Span raíz de la ejecución */
    span_t raiz;
    span_ini(&raiz, NULL, NULL, "wrapper", "comandos_so");

    if (argc > 1) {
        /* Usar los comandos seleccionados por el usuario en argv */
        for (int i = 1; i < argc; i++) {
            char *cmd[2] = { argv[i], NULL };
            ejecutar_y_trazar_comando(sock_fd, raiz.trace_id, raiz.span_id, cmd);
        }
    } else {
        /* Selección predeterminada de comandos reales del SO */
        char *c1[] = { "uname", "-a", NULL };
        char *c2[] = { "ls", "-la", "../include", NULL };
        char *c3[] = { "ps", "-e", NULL };
        char *c4[] = { "sleep", "0.05", NULL };
        char *c5[] = { "ls", "/ruta_inexistente_prueba", NULL }; /* genera error real */

        ejecutar_y_trazar_comando(sock_fd, raiz.trace_id, raiz.span_id, c1);
        ejecutar_y_trazar_comando(sock_fd, raiz.trace_id, raiz.span_id, c2);
        ejecutar_y_trazar_comando(sock_fd, raiz.trace_id, raiz.span_id, c3);
        ejecutar_y_trazar_comando(sock_fd, raiz.trace_id, raiz.span_id, c4);
        ejecutar_y_trazar_comando(sock_fd, raiz.trace_id, raiz.span_id, c5);
    }

    span_fin(&raiz, 0, "", "");
    escribir_exacto(sock_fd, &raiz, sizeof(span_t));

    /* Señal de cierre limpio al colector */
    span_t fin;
    memset(&fin, 0, sizeof(fin));
    snprintf(fin.servicio, sizeof(fin.servicio), "__FIN__");
    escribir_exacto(sock_fd, &fin, sizeof(span_t));
    close(sock_fd);

    int st_col = 0;
    waitpid(pid_col, &st_col, 0);
    return 0;
}
