#include "tabla.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* Módulo de formato e impresión jerárquica de trazas y spans en terminal */

#define C_RESET  "\033[0m"
#define C_BOLD   "\033[1m"
#define C_DIM    "\033[2m"
#define C_CYAN   "\033[36m"
#define C_GREEN  "\033[32m"
#define C_RED    "\033[31m"
#define C_YELLOW "\033[33m"

typedef struct {
    int cols;
    int ini_con_fecha;
    int w_ini;
    int w_fin;
    int w_id;
    int w_serv;
    int w_rel;
    int w_op;
    int w_dur;
    int w_casc;
    int w_pid;
    int w_msg;
} tabla_layout_t;

void tabla_formatear_relacion(const span_t *s, const char *destino,
                              const char *tipo, char *out, size_t out_sz) {
    if (out == NULL || out_sz == 0) return;
    if (s == NULL) {
        out[0] = '\0';
        return;
    }
    int es_http = (strncmp(s->operacion, "GET ", 4) == 0 ||
                   strncmp(s->operacion, "POST ", 5) == 0 ||
                   strncmp(s->operacion, "HTTP", 4) == 0);
    if (strcmp(s->servicio, "servicio_a") == 0) {
        if ((destino != NULL && strcmp(destino, "backend") == 0) || es_http) {
            snprintf(out, out_sz, "cliente_http -> backend");
        } else if (destino != NULL && strcmp(destino, "app") == 0) {
            snprintf(out, out_sz, "cliente_ipc -> app_viva");
        } else {
            snprintf(out, out_sz, "lanzador -> %.20s", s->operacion);
        }
    } else if (strcmp(s->servicio, "servicio_b") == 0) {
        if ((destino != NULL && strcmp(destino, "backend") == 0) || es_http) {
            snprintf(out, out_sz, "sensor_so -> backend");
        } else if (destino != NULL && strcmp(destino, "app") == 0) {
            snprintf(out, out_sz, "sensor_so -> app_viva");
        } else {
            snprintf(out, out_sz, "sensor_so -> %.20s", s->operacion);
        }
    } else if (strcmp(s->servicio, "tracelite") == 0) {
        snprintf(out, out_sz, "orquestador -> arbol_so");
    } else if (tipo != NULL && strcmp(tipo, "observacion") == 0) {
        snprintf(out, out_sz, "proceso_so -> pid:%d", s->pid);
    } else if (s->parent[0] != '\0') {
        snprintf(out, out_sz, "hijo_so -> %.20s", s->servicio);
    } else {
        snprintf(out, out_sz, "%.14s -> %.14s", s->servicio, s->operacion);
    }
}

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

static int obtener_ancho_terminal(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 20) {
        return (int)ws.ws_col;
    }
    const char *env_cols = getenv("COLUMNS");
    if (env_cols != NULL) {
        int c = atoi(env_cols);
        if (c > 20) return c;
    }
    return 120;
}

static tabla_layout_t calcular_layout(void) {
    tabla_layout_t l;
    l.cols = obtener_ancho_terminal();
    l.w_fin = 12;
    l.w_dur = 9;
    l.w_pid = 5;

    if (l.cols >= 165) {
        l.ini_con_fecha = 1;
        l.w_ini  = 23;
        l.w_id   = 16;
        l.w_serv = 20;
        l.w_rel  = 22;
        l.w_op   = 22;
        l.w_casc = 14;
    } else if (l.cols >= 125) {
        l.ini_con_fecha = 0;
        l.w_ini  = 12;
        l.w_id   = 12;
        l.w_serv = 18;
        l.w_rel  = 18;
        l.w_op   = 18;
        l.w_casc = 10;
    } else {
        l.ini_con_fecha = 0;
        l.w_ini  = 12;
        l.w_id   = 8;
        l.w_serv = 14;
        l.w_rel  = 14;
        l.w_op   = 14;
        l.w_casc = 8;
    }

    /* Ancho fijo previo a la columna ESTADO */
    int usado = l.w_ini + 1 + l.w_fin + 1 + l.w_id + 1 + l.w_id + 1 +
                l.w_serv + 1 + l.w_rel + 1 + l.w_op + 1 + l.w_dur + 2 +
                (l.w_casc + 2) + 2 + l.w_pid + 1 + 6;
    l.w_msg = l.cols - usado - 3;
    if (l.w_msg < 0) l.w_msg = 0;
    return l;
}

static void imprimir_separador(int ancho) {
    printf("%s", C_DIM);
    for (int i = 0; i < ancho; i++) {
        putchar('-');
    }
    printf("%s\n", C_RESET);
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

static void imprimir_arbol(const span_t *spans, int n_spans, int idx, int *impresos,
                           int nivel, int64_t traza_ini_ns, double total_ms,
                           int64_t delta_real_mono, const tabla_layout_t *l) {
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
    formatear_fecha_ns(s->ini + delta_real_mono, ini_fecha, sizeof(ini_fecha), l->ini_con_fecha);
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

    char cascada[32];
    construir_cascada(cascada, l->w_casc, offset_ms, s->dur, total_ms);

    int es_error = (s->estado != 0);
    int es_anom_tiempo = (!es_error && s->anomalia[0] != '\0');

    const char *color_est = es_error ? C_RED : (es_anom_tiempo ? C_YELLOW : C_GREEN);
    const char *color_bar = es_error ? C_RED : (es_anom_tiempo ? C_YELLOW : C_CYAN);
    const char *est_txt   = es_error ? "ERROR" : (es_anom_tiempo ? "ANOMALIA" : "OK");

    char rel_col[48];
    tabla_formatear_relacion(s, NULL, NULL, rel_col, sizeof(rel_col));

    printf("%-*.*s %-*.*s %-*.*s %-*.*s %-*.*s %-*.*s %-*.*s %*s  %s[%s]%s  %-*d %s%s",
           l->w_ini, l->w_ini, ini_fecha,
           l->w_fin, l->w_fin, fin_hora,
           l->w_id, l->w_id, s->span_id,
           l->w_id, l->w_id, parent_str,
           l->w_serv, l->w_serv, serv_col,
           l->w_rel, l->w_rel, rel_col,
           l->w_op, l->w_op, s->operacion,
           l->w_dur, dur_str,
           color_bar, cascada, C_RESET,
           l->w_pid, s->pid,
           color_est, est_txt);

    int extra_usado = (int)strlen(est_txt) - 6;
    if (extra_usado < 0) extra_usado = 0;

    if (s->anomalia[0] != '\0') {
        printf(" [%s]", s->anomalia);
        extra_usado += (int)strlen(s->anomalia) + 3;
    }

    int rem_msg = l->w_msg - extra_usado;
    if (s->msg[0] != '\0' && rem_msg >= 4) {
        printf(" (%.*s)", rem_msg, s->msg);
    }
    printf("%s\n", C_RESET);

    for (int i = 0; i < n_spans; i++) {
        if (!impresos[i] &&
            strcmp(spans[i].trace_id, s->trace_id) == 0 &&
            s->span_id[0] != '\0' &&
            strcmp(spans[i].parent, s->span_id) == 0) {
            imprimir_arbol(spans, n_spans, i, impresos, nivel + 1,
                           traza_ini_ns, total_ms, delta_real_mono, l);
        }
    }
}

int imprimir_trazas_spans(const span_t *spans, int n_spans) {
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

        tabla_layout_t l = calcular_layout();

        num_trazas++;
        printf("\n%s%sSolicitud %d | TRACE ID: %s%s  %s| Start: %s | Duracion total: %.2f ms%s\n",
               C_BOLD, C_YELLOW, num_trazas, tid, C_RESET, C_DIM, inicio_traza, total_ms, C_RESET);
        printf("%s%-*s %-*s %-*s %-*s %-*s %-*s %-*s %*s  %-*s  %-*s %s%s\n",
               C_BOLD,
               l.w_ini, "INICIO",
               l.w_fin, "FIN",
               l.w_id, "SPAN_ID",
               l.w_id, "PARENT_ID",
               l.w_serv, "SERVICIO",
               l.w_rel, "RELACION",
               l.w_op, "OPERACION",
               l.w_dur, "DURACION",
               l.w_casc + 2, "CASCADA",
               l.w_pid, "PID",
               "ESTADO", C_RESET);
        imprimir_separador(l.cols);

        for (int j = 0; j < n_spans; j++) {
            if (!impresos[j] &&
                strcmp(spans[j].trace_id, tid) == 0 &&
                spans[j].parent[0] == '\0') {
                imprimir_arbol(spans, n_spans, j, impresos, 0,
                               min_ini, total_ms, delta_real_mono, &l);
            }
        }
        for (int j = 0; j < n_spans; j++) {
            if (!impresos[j] && strcmp(spans[j].trace_id, tid) == 0) {
                imprimir_arbol(spans, n_spans, j, impresos, 1,
                               min_ini, total_ms, delta_real_mono, &l);
            }
        }
    }

    free(impresos);
    free(traza_vista);
    return num_trazas;
}

void imprimir_resumen_spans(const span_t *spans, int n_spans, int num_trazas) {
    int total_err = 0;
    int total_anom_t = 0;
    for (int i = 0; i < n_spans; i++) {
        if (spans[i].estado != 0) {
            total_err++;
        } else if (spans[i].anomalia[0] != '\0') {
            total_anom_t++;
        }
    }

    printf("\n%s-- Resumen: %d trazas, %d spans | %d errores, %d anomalias de tiempo --%s\n",
           C_BOLD, num_trazas, n_spans, total_err, total_anom_t, C_RESET);
}
