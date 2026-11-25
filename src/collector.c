#include "almacen.h"
#include "common.h"
#include "tabla.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* Colector concurrente (AF_UNIX + pthread): recibe spans, detecta anomalías e imprime la tabla */

static span_t *spans = NULL;
static int n_spans = 0;
static int cap_spans = 0;
static int terminar = 0;
static int server_fd = -1;
static pthread_mutex_t mutex_spans = PTHREAD_MUTEX_INITIALIZER;

/* Detección estadística de anomalías de tiempo (Leave-One-Out: dur > mu_-i + 2*sigma_-i) */
static void evaluar_anomalias_tiempo(void) {
    for (int i = 0; i < n_spans; i++) {
        if (spans[i].estado != 0 || spans[i].anomalia[0] != '\0') {
            continue;
        }
        if (strcmp(spans[i].servicio, "tracelite") == 0) {
            continue;
        }

        int mismos_serv = 0;
        int mismos_traza = 0;
        for (int j = 0; j < n_spans; j++) {
            if (j == i) continue;
            if (strcmp(spans[j].servicio, spans[i].servicio) == 0) {
                mismos_serv++;
            }
            if (strcmp(spans[j].servicio, "tracelite") != 0 &&
                strcmp(spans[j].trace_id, spans[i].trace_id) == 0) {
                mismos_traza++;
            }
        }

        int modo_ref = 0; /* 1: mismo servicio, 2: misma traza, 3: lote completo */
        if (mismos_serv >= 2) {
            modo_ref = 1;
        } else if (mismos_traza >= 2) {
            modo_ref = 2;
        } else {
            modo_ref = 3;
        }

        int n_ref = 0;
        double suma = 0.0;
        for (int j = 0; j < n_spans; j++) {
            if (j == i) continue;
            if (modo_ref == 1) {
                if (strcmp(spans[j].servicio, spans[i].servicio) != 0) continue;
            } else if (modo_ref == 2) {
                if (strcmp(spans[j].servicio, "tracelite") == 0 ||
                    strcmp(spans[j].trace_id, spans[i].trace_id) != 0) continue;
            } else {
                if (strcmp(spans[j].servicio, "tracelite") == 0) continue;
            }
            suma += spans[j].dur;
            n_ref++;
        }

        if (n_ref < 2) {
            continue;
        }

        double mu = suma / (double)n_ref;
        double var = 0.0;
        for (int j = 0; j < n_spans; j++) {
            if (j == i) continue;
            if (modo_ref == 1) {
                if (strcmp(spans[j].servicio, spans[i].servicio) != 0) continue;
            } else if (modo_ref == 2) {
                if (strcmp(spans[j].servicio, "tracelite") == 0 ||
                    strcmp(spans[j].trace_id, spans[i].trace_id) != 0) continue;
            } else {
                if (strcmp(spans[j].servicio, "tracelite") == 0) continue;
            }
            double diff = spans[j].dur - mu;
            var += diff * diff;
        }
        double sigma = sqrt(var / (double)n_ref);
        double umbral = mu + 2.0 * sigma;

        if (spans[i].dur > umbral &&
            spans[i].dur > 1.5 * mu &&
            (spans[i].dur - mu) >= 10.0) {
            snprintf(spans[i].anomalia, sizeof(spans[i].anomalia), "latencia_alta");
            if (spans[i].msg[0] == '\0') {
                snprintf(spans[i].msg, sizeof(spans[i].msg),
                         "mu=%.1fms umbral=%.1fms", mu, umbral);
            }
        }
    }
}

static void imprimir_todas_las_trazas(void) {
    evaluar_anomalias_tiempo();
    int num_trazas = imprimir_trazas_spans(spans, n_spans);
    imprimir_resumen_spans(spans, n_spans, num_trazas);

    almacen_indice_t alm;
    if (n_spans > 0 && almacen_cargar(&alm, NULL, 7, 0) == 0) {
        struct timespec ts_real, ts_mono;
        clock_gettime(CLOCK_REALTIME, &ts_real);
        clock_gettime(CLOCK_MONOTONIC, &ts_mono);
        int64_t delta = ((int64_t)ts_real.tv_sec * 1000000000LL + (int64_t)ts_real.tv_nsec) -
                        ((int64_t)ts_mono.tv_sec * 1000000000LL + (int64_t)ts_mono.tv_nsec);
        for (int i = 0; i < n_spans; i++) {
            almacen_meta_t meta;
            memset(&meta, 0, sizeof(meta));
            snprintf(meta.tipo, sizeof(meta.tipo), "observacion");
            snprintf(meta.modo, sizeof(meta.modo), "observacion");
            snprintf(meta.destino, sizeof(meta.destino), "proc");
            snprintf(meta.tiempo, sizeof(meta.tiempo), "normal");
            meta.ini_real_ns = spans[i].ini + delta;
            meta.fin_real_ns = spans[i].fin + delta;
            almacen_guardar_span(&alm, &spans[i], &meta);
        }
        almacen_liberar(&alm);
    }
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
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <ruta_socket>\n", argv[0]);
        return 1;
    }
    const char *sock_path = argv[1];
    if (validar_ruta_socket(sock_path) != 0) {
        return 1;
    }

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
