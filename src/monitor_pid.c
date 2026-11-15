#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* Agente observador por PID: lee /proc y envía su span al colector por Socket UNIX */

/* Lectura de comando completo desde /proc/<pid>/cmdline */
static void leer_cmdline_proc(int pid, const char *comm_fallback, char *out, size_t out_sz) {
    char ruta[64];
    snprintf(ruta, sizeof(ruta), "/proc/%d/cmdline", pid);
    FILE *f = fopen(ruta, "r");
    if (f != NULL) {
        size_t n = fread(out, 1, out_sz - 1, f);
        fclose(f);
        if (n > 0) {
            for (size_t i = 0; i < n; i++) {
                if (out[i] == '\0') {
                    out[i] = ' ';
                }
            }
            while (n > 0 && out[n - 1] == ' ') {
                n--;
            }
            out[n] = '\0';
            if (out[0] != '\0') {
                return;
            }
        }
    }
    snprintf(out, out_sz, "%s", comm_fallback);
}

static int leer_proc_stat(int pid, char *comm_out, size_t comm_sz, char *state_out) {
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
    if (sscanf(rparen + 2, "%c", &estado) != 1) {
        return -1;
    }
    *state_out = estado;
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

    span_t s;
    if (leer_proc_stat(pid_obj, comm, sizeof(comm), &state_ini) != 0) {
        span_ini(&s, trace_id, parent_id, "proc_inaccesible", "no_encontrado");
        snprintf(s.span_id, sizeof(s.span_id), "%s", own_span_id);
        s.pid = pid_obj;
        span_fin(&s, 1, "no_existe_en_proc", "error_proceso");
    } else {
        char op[48];
        leer_cmdline_proc(pid_obj, comm, op, sizeof(op));

        span_ini(&s, trace_id, parent_id, comm, op);
        snprintf(s.span_id, sizeof(s.span_id), "%s", own_span_id);
        s.pid = pid_obj;

        dormir_ms(dur_ms);

        char comm_fin[24];
        char state_fin = '?';
        if (leer_proc_stat(pid_obj, comm_fin, sizeof(comm_fin), &state_fin) != 0) {
            span_fin(&s, 1, "proceso_termino", "error_proceso");
        } else {
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
