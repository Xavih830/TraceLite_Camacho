#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/*
 * Agente observador (monitor_pid): lanzado mediante fork() + execv() por cada PID vivo
 * en la jerarquía descubierta. Lee /proc/<pid>/comm y /proc/<pid>/stat al inicio y al
 * final de la ventana de muestreo, y envía un span_t con su own_span_id y parent_id
 * reales al colector vía Socket UNIX.
 *
 * Uso: ./monitor_pid <pid> <trace_id> <own_span_id> <parent_id> <sock_path> <ms>
 */

static int leer_proc_stat(int pid, char *comm_out, size_t comm_sz,
                          char *state_out, unsigned long *ticks_out) {
    char ruta_comm[64];
    snprintf(ruta_comm, sizeof(ruta_comm), "/proc/%d/comm", pid);
    FILE *fc = fopen(ruta_comm, "r");
    if (fc == NULL) {
        return -1;
    }
    if (fgets(comm_out, (int)comm_sz, fc) == NULL) {
        fclose(fc);
        return -1;
    }
    fclose(fc);
    size_t len = strlen(comm_out);
    if (len > 0 && comm_out[len - 1] == '\n') {
        comm_out[len - 1] = '\0';
    }

    char ruta_stat[64];
    snprintf(ruta_stat, sizeof(ruta_stat), "/proc/%d/stat", pid);
    FILE *fs = fopen(ruta_stat, "r");
    if (fs == NULL) {
        return -1;
    }
    char linea[1024];
    if (fgets(linea, sizeof(linea), fs) == NULL) {
        fclose(fs);
        return -1;
    }
    fclose(fs);

    char *rparen = strrchr(linea, ')');
    if (rparen == NULL || rparen[1] != ' ') {
        return -1;
    }

    char estado = '?';
    unsigned long utime = 0, stime = 0;
    int leidos = sscanf(rparen + 2,
                        "%c %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %lu %lu",
                        &estado, &utime, &stime);
    if (leidos < 3) {
        return -1;
    }

    *state_out = estado;
    *ticks_out = utime + stime;
    return 0;
}

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

int main(int argc, char *argv[]) {
    if (argc < 7) {
        fprintf(stderr, "Uso: %s <pid> <trace_id> <own_span_id> <parent_id> <sock_path> <ms>\n", argv[0]);
        return 1;
    }

    int pid_obj = atoi(argv[1]);
    const char *trace_id = argv[2];
    const char *own_span_id = argv[3];
    const char *parent_id = argv[4];
    const char *sock_path = argv[5];
    int dur_ms = atoi(argv[6]);

    char comm[24] = "desconocido";
    char state_ini = '?';
    unsigned long ticks_ini = 0;

    span_t s;
    if (leer_proc_stat(pid_obj, comm, sizeof(comm), &state_ini, &ticks_ini) != 0) {
        span_ini(&s, trace_id, parent_id, "proc_inaccesible", "no_encontrado");
        snprintf(s.span_id, sizeof(s.span_id), "%s", own_span_id);
        s.pid = pid_obj;
        span_fin(&s, 1, "no_existe_en_proc", "");
    } else {
        span_ini(&s, trace_id, parent_id, comm, "observando");
        snprintf(s.span_id, sizeof(s.span_id), "%s", own_span_id);
        s.pid = pid_obj;

        dormir_ms(dur_ms);

        char comm_fin[24];
        char state_fin = '?';
        unsigned long ticks_fin = 0;
        if (leer_proc_stat(pid_obj, comm_fin, sizeof(comm_fin), &state_fin, &ticks_fin) != 0) {
            snprintf(s.operacion, sizeof(s.operacion), "estado:%c->EXIT", state_ini);
            span_fin(&s, 1, "proceso_termino", "");
        } else {
            unsigned long delta_ticks = (ticks_fin >= ticks_ini) ? (ticks_fin - ticks_ini) : 0;
            snprintf(s.operacion, sizeof(s.operacion), "estado:%c->%c ticks:+%lu",
                     state_ini, state_fin, delta_ticks);
            span_fin(&s, 0, "", "");
        }
    }

    int sock_fd = conectar_colector(sock_path);
    if (sock_fd < 0) {
        return 1;
    }
    escribir_exacto(sock_fd, &s, sizeof(span_t));
    close(sock_fd);
    return 0;
}
