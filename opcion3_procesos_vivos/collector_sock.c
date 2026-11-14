#include "common.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/*
 * Colector concurrente basado en Sockets UNIX (AF_UNIX) + Hilos (pthread)
 * para la Opción 3 (observación de procesos vivos del SO).
 * Muestra Trace ID, Span ID, Parent Span ID, Servicio, Operación,
 * Inicio (fecha/hora), Fin (hora/ms), Duración, Cascada, PID y Estado.
 */

#define C_RESET  "\033[0m"
#define C_BOLD   "\033[1m"
#define C_DIM    "\033[2m"
#define C_CYAN   "\033[36m"
#define C_GREEN  "\033[32m"
#define C_RED    "\033[31m"
#define C_YELLOW "\033[33m"

static span_t *spans = NULL;
static int n_spans = 0;
static int cap_spans = 0;
static int terminar = 0;
static int server_fd = -1;
static pthread_mutex_t mutex_spans = PTHREAD_MUTEX_INITIALIZER;

static void formatear_fecha_ns(int64_t real_ns, char *buf, size_t sz, int incluir_fecha) {
    if (real_ns < 0) real_ns = 0;
    time_t seg = (time_t)(real_ns / 1000000000LL);
    long ms = (long)((real_ns % 1000000000LL) / 1000000LL);
    struct tm tm_info;
    localtime_r(&seg, &tm_info);
    size_t usado = strftime(buf, sz, incluir_fecha ? "%Y-%m-%d %H:%M:%S" : "%H:%M:%S", &tm_info);
    if (usado > 0 && usado + 5 <= sz) {
        snprintf(buf + usado, sz - usado, ".%03ld", ms);
    }
}

static void construir_cascada(char *buf, int ancho, double offset_ms,
                              double dur_ms, double total_ms) {
    if (total_ms <= 0.0) {
        total_ms = 1.0;
    }
    double fin_ms = offset_ms + dur_ms;
    for (int i = 0; i < ancho; i++) {
        double bin_ini = ((double)i / ancho) * total_ms;
        double bin_fin = ((double)(i + 1) / ancho) * total_ms;
        if (fin_ms > bin_ini && offset_ms < bin_fin) {
            buf[i] = '#';
        } else {
            buf[i] = '.';
        }
    }
    buf[ancho] = '\0';
}

static void imprimir_arbol(int idx, int *impresos, int nivel,
                           int64_t traza_ini_ns, double total_ms,
                           int64_t delta_real_mono) {
    if (idx < 0 || idx >= n_spans || impresos[idx] || nivel > 32) {
        return;
    }
    impresos[idx] = 1;

    const span_t *s = &spans[idx];

    double offset_ms = (double)(s->ini - traza_ini_ns) / 1000000.0;
    if (offset_ms < 0.0) {
        offset_ms = 0.0;
    }

    char ini_fecha[32];
    char fin_hora[20];
    formatear_fecha_ns(s->ini + delta_real_mono, ini_fecha, sizeof(ini_fecha), 1);
    formatear_fecha_ns(s->fin + delta_real_mono, fin_hora, sizeof(fin_hora), 0);

    const char *parent_str = (s->parent[0] != '\0') ? s->parent : "(raiz)";

    char serv_col[36];
    if (nivel == 0) {
        snprintf(serv_col, sizeof(serv_col), "%s", s->servicio);
    } else {
        int pos = 0;
        for (int i = 1; i < nivel && pos < 16; i++) {
            pos += snprintf(serv_col + pos, sizeof(serv_col) - (size_t)pos, "  ");
        }
        snprintf(serv_col + pos, sizeof(serv_col) - (size_t)pos, "`- %s", s->servicio);
    }

    char dur_str[16];
    snprintf(dur_str, sizeof(dur_str), "%.2f ms", s->dur);

    char cascada[17];
    construir_cascada(cascada, 16, offset_ms, s->dur, total_ms);

    const char *color_est = (s->estado == 0) ? C_GREEN : C_RED;
    const char *color_bar = (s->estado == 0) ? C_CYAN : C_RED;
    const char *est_txt   = (s->estado == 0) ? "OK" : "ERROR";

    printf("%-23s %-12s %-16s %-16s %-24.24s %-26.26s %9s  %s[%s]%s  %-5d %s%s",
           ini_fecha,
           fin_hora,
           s->span_id,
           parent_str,
           serv_col,
           s->operacion,
           dur_str,
           color_bar, cascada, C_RESET,
           s->pid,
           color_est, est_txt);

    if (s->msg[0] != '\0') {
        printf(" (%s)", s->msg);
    }
    printf("%s\n", C_RESET);

    for (int i = 0; i < n_spans; i++) {
        if (!impresos[i] &&
            strcmp(spans[i].trace_id, s->trace_id) == 0 &&
            s->span_id[0] != '\0' &&
            strcmp(spans[i].parent, s->span_id) == 0) {
            imprimir_arbol(i, impresos, nivel + 1, traza_ini_ns, total_ms, delta_real_mono);
        }
    }
}

static void imprimir_todas_las_trazas(void) {
    struct timespec ts_real, ts_mono;
    clock_gettime(CLOCK_REALTIME, &ts_real);
    clock_gettime(CLOCK_MONOTONIC, &ts_mono);
    int64_t real_ns = (int64_t)ts_real.tv_sec * 1000000000LL + (int64_t)ts_real.tv_nsec;
    int64_t mono_ns = (int64_t)ts_mono.tv_sec * 1000000000LL + (int64_t)ts_mono.tv_nsec;
    int64_t delta_real_mono = real_ns - mono_ns;

    int *impresos = NULL;
    int *traza_vista = NULL;
    if (n_spans > 0) {
        impresos = (int *)calloc((size_t)n_spans, sizeof(int));
        traza_vista = (int *)calloc((size_t)n_spans, sizeof(int));
    }

    int num_trazas = 0;
    for (int i = 0; i < n_spans; i++) {
        if (traza_vista[i]) {
            continue;
        }
        const char *tid = spans[i].trace_id;
        int64_t min_ini = spans[i].ini;
        int64_t max_fin = spans[i].fin;

        for (int j = i; j < n_spans; j++) {
            if (strcmp(spans[j].trace_id, tid) == 0) {
                traza_vista[j] = 1;
                if (spans[j].ini < min_ini) min_ini = spans[j].ini;
                if (spans[j].fin > max_fin) max_fin = spans[j].fin;
            }
        }

        double total_ms = (double)(max_fin - min_ini) / 1000000.0;
        if (total_ms <= 0.0) total_ms = 1.0;

        char inicio_traza[32];
        formatear_fecha_ns(min_ini + delta_real_mono, inicio_traza, sizeof(inicio_traza), 1);

        num_trazas++;
        printf("\n%s%sTRACE ID: %s%s  %s| Start: %s | Duracion total: %.2f ms%s\n",
               C_BOLD, C_YELLOW, tid, C_RESET, C_DIM, inicio_traza, total_ms, C_RESET);
        printf("%s%-23s %-12s %-16s %-16s %-24s %-26s %9s  %-18s  %-5s %s%s\n",
               C_BOLD,
               "INICIO", "FIN", "SPAN_ID", "PARENT_ID", "SERVICIO", "OPERACION",
               "DURACION", "CASCADA", "PID", "ESTADO", C_RESET);
        printf("%s--------------------------------------------------------------------------------------------------------------------------------------------------------------------------%s\n",
               C_DIM, C_RESET);

        for (int j = 0; j < n_spans; j++) {
            if (!impresos[j] &&
                strcmp(spans[j].trace_id, tid) == 0 &&
                spans[j].parent[0] == '\0') {
                imprimir_arbol(j, impresos, 0, min_ini, total_ms, delta_real_mono);
            }
        }
        for (int j = 0; j < n_spans; j++) {
            if (!impresos[j] && strcmp(spans[j].trace_id, tid) == 0) {
                imprimir_arbol(j, impresos, 1, min_ini, total_ms, delta_real_mono);
            }
        }
    }

    printf("\n%s-- Resumen: %d trazas, %d spans --%s\n",
           C_BOLD, num_trazas, n_spans, C_RESET);
    free(impresos);
    free(traza_vista);
}

static void *atender_cliente(void *arg) {
    int client_fd = *(int *)arg;
    free(arg);

    span_t temp;
    while (leer_exacto(client_fd, &temp, sizeof(span_t)) == 1) {
        pthread_mutex_lock(&mutex_spans);
        if (strcmp(temp.servicio, "__FIN__") == 0) {
            terminar = 1;
            if (server_fd >= 0) {
                shutdown(server_fd, SHUT_RDWR);
            }
            pthread_mutex_unlock(&mutex_spans);
            break;
        }

        if (n_spans >= cap_spans) {
            int nueva_cap = (cap_spans == 0) ? 16 : cap_spans * 2;
            span_t *nuevo = (span_t *)realloc(spans, (size_t)nueva_cap * sizeof(span_t));
            if (nuevo != NULL) {
                spans = nuevo;
                cap_spans = nueva_cap;
            }
        }
        if (n_spans < cap_spans) {
            spans[n_spans++] = temp;
        }
        pthread_mutex_unlock(&mutex_spans);
    }

    close(client_fd);
    return NULL;
}

int main(int argc, char *argv[]) {
    const char *sock_path = (argc > 1) ? argv[1] : "/tmp/tracelite_op3.sock";

    unlink(sock_path);

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 16) < 0) {
        perror("listen");
        close(server_fd);
        unlink(sock_path);
        return 1;
    }

    pthread_t *hilos = NULL;
    int n_hilos = 0;
    int cap_hilos = 0;

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            pthread_mutex_lock(&mutex_spans);
            int fin = terminar;
            pthread_mutex_unlock(&mutex_spans);
            if (fin) {
                break;
            }
            continue;
        }

        int *fd_ptr = (int *)malloc(sizeof(int));
        if (fd_ptr == NULL) {
            close(client_fd);
            continue;
        }
        *fd_ptr = client_fd;

        pthread_t tid;
        if (pthread_create(&tid, NULL, atender_cliente, fd_ptr) != 0) {
            free(fd_ptr);
            close(client_fd);
            continue;
        }

        if (n_hilos >= cap_hilos) {
            int nueva_cap = (cap_hilos == 0) ? 16 : cap_hilos * 2;
            pthread_t *nuevo = (pthread_t *)realloc(hilos, (size_t)nueva_cap * sizeof(pthread_t));
            if (nuevo != NULL) {
                hilos = nuevo;
                cap_hilos = nueva_cap;
            }
        }
        if (n_hilos < cap_hilos) {
            hilos[n_hilos++] = tid;
        } else {
            pthread_detach(tid);
        }
    }

    for (int i = 0; i < n_hilos; i++) {
        pthread_join(hilos[i], NULL);
    }

    imprimir_todas_las_trazas();

    free(hilos);
    free(spans);
    close(server_fd);
    unlink(sock_path);
    return 0;
}
