#include "solicitudes.h"
#include "almacen.h"
#include "common.h"
#include "protocolo.h"
#include "tabla.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Collector Lite en el proceso principal: orquesta A/B, recibe spans y muestra trazas */

static span_t *g_spans = NULL;
static int g_n_spans = 0;
static int g_cap_spans = 0;
static int g_srv_spans = -1;
static int g_cerrar_srv = 0;
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;

static void esperar_pid_seguro(pid_t pid) {
    int st = 0;
    if (pid <= 0) return;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
}

static void *hilo_cliente_spans(void *arg) {
    int cfd = *(int *)arg;
    free(arg);

    span_t s;
    while (leer_exacto(cfd, &s, sizeof(s)) == 1) {
        pthread_mutex_lock(&g_mtx);
        if (g_n_spans >= g_cap_spans) {
            int ncap = (g_cap_spans == 0) ? 16 : g_cap_spans * 2;
            span_t *nuevo = (span_t *)realloc(g_spans, (size_t)ncap * sizeof(span_t));
            if (nuevo != NULL) {
                g_spans = nuevo;
                g_cap_spans = ncap;
            }
        }
        if (g_n_spans < g_cap_spans) {
            g_spans[g_n_spans++] = s;
        }
        pthread_mutex_unlock(&g_mtx);
    }
    close(cfd);
    return NULL;
}

static void *hilo_aceptar_spans(void *arg) {
    (void)arg;
    pthread_t *hilos = NULL;
    int nh = 0, caph = 0;

    while (1) {
        int cfd = aceptar_unix(g_srv_spans, 0);
        if (cfd < 0) {
            pthread_mutex_lock(&g_mtx);
            int fin = g_cerrar_srv;
            pthread_mutex_unlock(&g_mtx);
            if (fin) break;
            if (errno == EINTR) continue;
            break;
        }
        int *pfd = (int *)malloc(sizeof(int));
        if (pfd == NULL) {
            close(cfd);
            continue;
        }
        *pfd = cfd;
        pthread_t tid;
        if (pthread_create(&tid, NULL, hilo_cliente_spans, pfd) != 0) {
            free(pfd);
            close(cfd);
            continue;
        }
        if (nh >= caph) {
            int nc = (caph == 0) ? 8 : caph * 2;
            pthread_t *nuevo = (pthread_t *)realloc(hilos, (size_t)nc * sizeof(pthread_t));
            if (nuevo != NULL) {
                hilos = nuevo;
                caph = nc;
            }
        }
        if (nh < caph) {
            hilos[nh++] = tid;
        } else {
            pthread_detach(tid);
        }
    }

    for (int i = 0; i < nh; i++) {
        pthread_join(hilos[i], NULL);
    }
    free(hilos);
    return NULL;
}

static const char *nombre_modo(modo_t m) {
    switch (m) {
        case MODO_RESPUESTA:   return "respuesta";
        case MODO_ESTABILIDAD: return "estabilidad";
        case MODO_ESCALADO:    return "escalado";
        case MODO_FALLO:       return "fallo";
    }
    return "respuesta";
}

static const char *nombre_tiempo(tiempo_nivel_t t) {
    switch (t) {
        case TIEMPO_NORMAL:   return "normal";
        case TIEMPO_ALTO:     return "alto";
        case TIEMPO_MUY_ALTO: return "muy-alto";
        case TIEMPO_MANUAL:   return "manual";
    }
    return "normal";
}

static const char *nombre_destino(destino_t d) {
    switch (d) {
        case DEST_PROGRAMA: return "programa";
        case DEST_BACKEND:  return "backend";
        case DEST_APP:      return "app";
    }
    return "programa";
}

static int dividir_csv(const char *csv, char items[][128], int max_items) {
    if (csv == NULL || csv[0] == '\0') return 0;
    char copia[1024];
    snprintf(copia, sizeof(copia), "%s", csv);
    int n = 0;
    char *p = copia;
    while (p != NULL && *p != '\0' && n < max_items) {
        char *coma = strchr(p, ',');
        if (coma != NULL) *coma = '\0';
        snprintf(items[n++], 128, "%.127s", p);
        p = (coma != NULL) ? (coma + 1) : NULL;
    }
    return n;
}

static void construir_linea_solicitud(const solicitudes_cfg_t *cfg, int idx_1,
                                      const char *entrada_i, char *out, size_t out_sz) {
    if (cfg->modo == MODO_FALLO && idx_1 == cfg->fallo_en) {
        if (cfg->destino == DEST_BACKEND) {
            snprintf(out, out_sz, "/ruta_inexistente_tracelite");
            return;
        }
        char copia[512];
        snprintf(copia, sizeof(copia), "%s", cfg->cmd_linea ? cfg->cmd_linea : "");
        char *ult_sp = strrchr(copia, ' ');
        if (ult_sp != NULL && ult_sp != copia) {
            *ult_sp = '\0';
        }
        snprintf(out, out_sz, "%.400s archivo_inexistente_tracelite", copia);
        return;
    }

    if (entrada_i != NULL && entrada_i[0] != '\0') {
        if (cfg->destino == DEST_BACKEND) {
            const char *base = cfg->ruta_http ? cfg->ruta_http : "/";
            const char *sep = strchr(base, '?') ? "&" : "?";
            snprintf(out, out_sz, "%.255s%s%.127s", base, sep, entrada_i);
            return;
        }
        char copia[512];
        snprintf(copia, sizeof(copia), "%s", cfg->cmd_linea ? cfg->cmd_linea : "");
        char *ult_sp = strrchr(copia, ' ');
        if (ult_sp != NULL) {
            *ult_sp = '\0';
        }
        snprintf(out, out_sz, "%.350s %.127s", copia, entrada_i);
        return;
    }

    const char *def = (cfg->destino == DEST_BACKEND)
                      ? (cfg->ruta_http ? cfg->ruta_http : "/")
                      : (cfg->cmd_linea ? cfg->cmd_linea : "");
    snprintf(out, out_sz, "%s", def);
}

static const char *clase_latencia(double dur_ms) {
    if (dur_ms <= 100.0) return "instantanea";
    if (dur_ms <= 1000.0) return "fluida";
    if (dur_ms <= 10000.0) return "atencion";
    return "excedida";
}

static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

static int contar_spans_de_traza(const char *trace_id) {
    int c = 0;
    pthread_mutex_lock(&g_mtx);
    for (int i = 0; i < g_n_spans; i++) {
        if (strcmp(g_spans[i].trace_id, trace_id) == 0) {
            c++;
        }
    }
    pthread_mutex_unlock(&g_mtx);
    return c;
}

static int evaluar_y_alertar_solicitud(const solicitudes_cfg_t *cfg,
                                       const almacen_indice_t *alm, int alm_ok,
                                       const char *trace_id) {
    int idx_a = -1, idx_b = -1;
    pthread_mutex_lock(&g_mtx);
    for (int i = 0; i < g_n_spans; i++) {
        if (strcmp(g_spans[i].trace_id, trace_id) != 0) continue;
        if (strcmp(g_spans[i].servicio, "servicio_a") == 0) idx_a = i;
        else if (strcmp(g_spans[i].servicio, "servicio_b") == 0) idx_b = i;
    }

    int es_tty = isatty(STDOUT_FILENO);
    const char *c_ini = es_tty ? "\033[31m" : "";
    const char *c_fin = es_tty ? "\033[0m" : "";
    const char *mstr = nombre_modo(cfg->modo);

    if (idx_a >= 0 && cfg->modo == MODO_RESPUESTA && g_spans[idx_a].estado == 0) {
        const char *cl = clase_latencia(g_spans[idx_a].dur);
        char tmp_msg[64];
        if (g_spans[idx_a].msg[0] != '\0') {
            snprintf(tmp_msg, sizeof(tmp_msg), "%.44s [%s]", g_spans[idx_a].msg, cl);
        } else {
            snprintf(tmp_msg, sizeof(tmp_msg), "[%s]", cl);
        }
        snprintf(g_spans[idx_a].msg, sizeof(g_spans[idx_a].msg), "%s", tmp_msg);
    }

    int idx_err = -1;
    if (idx_b >= 0 && g_spans[idx_b].estado != 0) idx_err = idx_b;
    else if (idx_a >= 0 && g_spans[idx_a].estado != 0) idx_err = idx_a;

    if (idx_err >= 0) {
        if (g_spans[idx_err].anomalia[0] == '\0') {
            snprintf(g_spans[idx_err].anomalia, sizeof(g_spans[idx_err].anomalia), "error_proceso");
        }
        printf("%s[ALERTA] servicio=%s trace_id=%s tipo=error_proceso operacion=%s dur=%.2fms modo=%s%s\n",
               c_ini, g_spans[idx_err].servicio, trace_id,
               g_spans[idx_err].operacion, g_spans[idx_err].dur, mstr, c_fin);
        fflush(stdout);
        pthread_mutex_unlock(&g_mtx);
        return 1;
    }

    if (idx_b < 0) {
        pthread_mutex_unlock(&g_mtx);
        return 0;
    }

    span_t *sb = (cfg->ventana_seg > 0 && idx_a >= 0) ? &g_spans[idx_a] : &g_spans[idx_b];
    if (cfg->modo == MODO_RESPUESTA) {
        double slo = cfg->slo_ms;
        if (slo <= 0.0) {
            slo = (cfg->destino == DEST_APP) ? 200.0 : 1000.0;
        }
        if (sb->dur > slo) {
            snprintf(sb->anomalia, sizeof(sb->anomalia), "slo_excedido");
            snprintf(sb->msg, sizeof(sb->msg), "slo=%.0fms", slo);
            printf("%s[ALERTA] servicio=%s trace_id=%s tipo=slo_excedido operacion=%s "
                   "dur=%.2fms slo=%.2fms modo=%s%s\n",
                   c_ini, sb->servicio, trace_id, sb->operacion, sb->dur, slo, mstr, c_fin);
            fflush(stdout);
            pthread_mutex_unlock(&g_mtx);
            return 1;
        }
        pthread_mutex_unlock(&g_mtx);
        return 0;
    }

    if (cfg->modo != MODO_ESTABILIDAD) {
        pthread_mutex_unlock(&g_mtx);
        return 0;
    }

    double refs[50];
    int n_ref = 0;
    /* Incluir otros spans OK del lote actual (para evaluación al cierre del lote) */
    for (int i = g_n_spans - 1; i >= 0 && n_ref < 50; i--) {
        if (i == idx_b) continue;
        if (strcmp(g_spans[i].servicio, sb->servicio) != 0) continue;
        if (strcmp(g_spans[i].operacion, sb->operacion) != 0) continue;
        if (strcmp(g_spans[i].atribucion, sb->atribucion) != 0) continue;
        if (g_spans[i].estado != 0 || g_spans[i].anomalia[0] != '\0') continue;
        refs[n_ref++] = g_spans[i].dur;
    }
    if (alm_ok) {
        for (int i = alm->n - 1; i >= 0 && n_ref < 50; i--) {
            const almacen_registro_t *r = &alm->regs[i];
            if (strcmp(r->meta.tipo, "solicitud") != 0) continue;
            if (strcmp(r->meta.modo, mstr) != 0) continue;
            if (strcmp(r->meta.destino, nombre_destino(cfg->destino)) != 0) continue;
            if (r->span.estado != 0 || r->span.anomalia[0] != '\0') continue;
            if (strcmp(r->span.servicio, sb->servicio) != 0) continue;
            if (strcmp(r->span.operacion, sb->operacion) != 0) continue;
            if (strcmp(r->span.atribucion, sb->atribucion) != 0) continue;
            refs[n_ref++] = r->span.dur;
        }
    }

    if (n_ref < 3) {
        pthread_mutex_unlock(&g_mtx);
        return 0;
    }

    double suma = 0.0;
    for (int i = 0; i < n_ref; i++) suma += refs[i];
    double mu = suma / (double)n_ref;
    double var = 0.0;
    for (int i = 0; i < n_ref; i++) {
        double d = refs[i] - mu;
        var += d * d;
    }
    double sigma = sqrt(var / (double)n_ref);
    double umbral = mu + 2.0 * sigma;

    int alerta = 0;
    if (sb->dur > umbral && sb->dur > 1.5 * mu && (sb->dur - mu) >= 10.0) {
        snprintf(sb->anomalia, sizeof(sb->anomalia), "latencia_alta");
        snprintf(sb->msg, sizeof(sb->msg), "mu=%.2fms umbral=%.2fms", mu, umbral);
        printf("%s[ALERTA] servicio=%s trace_id=%s tipo=latencia_alta operacion=%s "
               "dur=%.2fms mu=%.2fms umbral=%.2fms modo=%s%s\n",
               c_ini, sb->servicio, trace_id, sb->operacion, sb->dur, mu, umbral, mstr, c_fin);
        fflush(stdout);
        alerta = 1;
    }
    pthread_mutex_unlock(&g_mtx);
    return alerta;
}

static int evaluar_escalado_lote(const double *pesos, int n_pesos,
                                 const char trace_ids[][33], int n_trazas) {
    int alertas = 0;
    int es_tty = isatty(STDOUT_FILENO);
    const char *c_ini = es_tty ? "\033[31m" : "";
    const char *c_fin = es_tty ? "\033[0m" : "";

    pthread_mutex_lock(&g_mtx);
    double ratios[32];
    int b_idx[32];
    int ok_count = 0;
    for (int t = 0; t < n_trazas && t < 32; t++) {
        b_idx[t] = -1;
        ratios[t] = 0.0;
        for (int i = 0; i < g_n_spans; i++) {
            if (strcmp(g_spans[i].trace_id, trace_ids[t]) == 0 &&
                strcmp(g_spans[i].servicio, "servicio_b") == 0) {
                b_idx[t] = i;
                if (g_spans[i].estado == 0 && t < n_pesos && pesos[t] > 0.0) {
                    ratios[t] = g_spans[i].dur / pesos[t];
                    ok_count++;
                }
                break;
            }
        }
    }

    if (ok_count >= 3) {
        for (int t = 0; t < n_trazas && t < 32; t++) {
            int idx = b_idx[t];
            if (idx < 0 || g_spans[idx].estado != 0 || t >= n_pesos || pesos[t] <= 0.0) continue;
            double otros[32];
            int no = 0;
            for (int j = 0; j < n_trazas && j < 32; j++) {
                if (j == t || b_idx[j] < 0 || g_spans[b_idx[j]].estado != 0) continue;
                if (j < n_pesos && pesos[j] > 0.0) otros[no++] = ratios[j];
            }
            if (no < 2) continue;
            qsort(otros, (size_t)no, sizeof(double), cmp_double);
            double med = (no % 2 == 1) ? otros[no / 2] : 0.5 * (otros[no / 2 - 1] + otros[no / 2]);
            if (ratios[t] > 1.5 * med && (g_spans[idx].dur - 1.5 * med * pesos[t]) >= 10.0) {
                snprintf(g_spans[idx].anomalia, sizeof(g_spans[idx].anomalia), "escalado_no_lineal");
                snprintf(g_spans[idx].msg, sizeof(g_spans[idx].msg),
                         "ratio=%.1f med=%.1f", ratios[t], med);
                printf("%s[ALERTA] servicio=%s trace_id=%s tipo=escalado_no_lineal operacion=%s "
                       "dur=%.2fms modo=escalado%s\n",
                       c_ini, g_spans[idx].servicio, trace_ids[t],
                       g_spans[idx].operacion, g_spans[idx].dur, c_fin);
                fflush(stdout);
                alertas++;
            }
        }
    }
    pthread_mutex_unlock(&g_mtx);
    return alertas;
}

static int cargar_lineas_lote(const char *ruta, char ***out_lineas, int *out_n) {
    FILE *f = fopen(ruta, "r");
    if (f == NULL) {
        perror("fopen lote");
        return -1;
    }
    char **lineas = NULL;
    int n = 0, cap = 0;
    char buf[512];
    while (fgets(buf, sizeof(buf), f) != NULL) {
        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
            buf[--len] = '\0';
        }
        char *p = buf;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#') {
            continue;
        }
        if (n >= cap) {
            int nc = (cap == 0) ? 8 : cap * 2;
            char **nuevo = (char **)realloc(lineas, (size_t)nc * sizeof(char *));
            if (nuevo == NULL) {
                fclose(f);
                return -1;
            }
            lineas = nuevo;
            cap = nc;
        }
        lineas[n] = strdup(p);
        n++;
    }
    fclose(f);
    *out_lineas = lineas;
    *out_n = n;
    return 0;
}

static int esperar_tcp_listo(const char *host, int puerto, int max_ms) {
    int transcurrido = 0;
    while (transcurrido < max_ms) {
        int sfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (sfd >= 0) {
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons((uint16_t)puerto);
            inet_pton(AF_INET, host, &addr.sin_addr);
            if (connect(sfd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                close(sfd);
                return 0;
            }
            close(sfd);
        }
        dormir_ms(50);
        transcurrido += 50;
    }
    return -1;
}

static pid_t lanzar_servicio_cmd(const char *cmd_str, int mostrar_salida) {
    char copia[512];
    snprintf(copia, sizeof(copia), "%s", cmd_str);
    char *argv_cmd[64];
    int argc = 0;
    char *p = copia;
    while (*p != '\0' && argc < 63) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;
        argv_cmd[argc++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\t') p++;
        if (*p != '\0') *p++ = '\0';
    }
    argv_cmd[argc] = NULL;
    if (argc == 0) return -1;

    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        if (!mostrar_salida) {
            int dn = open("/dev/null", O_WRONLY);
            if (dn >= 0) {
                dup2(dn, STDOUT_FILENO);
                dup2(dn, STDERR_FILENO);
                close(dn);
            }
        }
        execvp(argv_cmd[0], argv_cmd);
        _exit(127);
    }
    if (pid > 0) setpgid(pid, pid);
    return pid;
}

static void detener_servicio_nuevo(pid_t pid) {
    if (pid <= 0) return;
    kill(-pid, SIGTERM);
    int espera = 0, st = 0;
    while (espera < 2000) {
        if (waitpid(pid, &st, WNOHANG) == pid) return;
        dormir_ms(20);
        espera += 20;
    }
    kill(-pid, SIGKILL);
    esperar_pid_seguro(pid);
}

int ejecutar_solicitudes(const solicitudes_cfg_t *cfg,
                         const char *dir_priv,
                         const char *spans_sock,
                         volatile sig_atomic_t *detener_flag) {
    char ruta_a[PATH_MAX], ruta_b[PATH_MAX];
    char a_sock[PATH_MAX], b_sock[PATH_MAX];
    if (ruta_hermana("servicio_a", ruta_a, sizeof(ruta_a)) != 0 ||
        ruta_hermana("servicio_b", ruta_b, sizeof(ruta_b)) != 0) {
        fprintf(stderr, "[tracelite] No se pudieron resolver rutas de servicio_a/servicio_b.\n");
        return 1;
    }

    int na = snprintf(a_sock, sizeof(a_sock), "%s/a.sock", dir_priv);
    int nb = snprintf(b_sock, sizeof(b_sock), "%s/b.sock", dir_priv);
    if (na < 0 || (size_t)na >= sizeof(a_sock) || validar_ruta_socket(a_sock) != 0 ||
        nb < 0 || (size_t)nb >= sizeof(b_sock) || validar_ruta_socket(b_sock) != 0) {
        return 1;
    }

    char **lineas_lote = NULL;
    int n_lote = 0;
    if (cfg->lote_path != NULL) {
        if (cargar_lineas_lote(cfg->lote_path, &lineas_lote, &n_lote) != 0 || n_lote == 0) {
            fprintf(stderr, "[tracelite] Lote vacío o ilegible: %s\n", cfg->lote_path);
            for (int i = 0; i < n_lote; i++) free(lineas_lote[i]);
            free(lineas_lote);
            return 1;
        }
    }

    g_spans = NULL;
    g_n_spans = 0;
    g_cap_spans = 0;
    g_cerrar_srv = 0;

    almacen_indice_t alm;
    int alm_ok = (almacen_cargar(&alm, NULL, cfg->ret_dias, cfg->ret_seg) == 0);

    struct timespec ts_real, ts_mono;
    clock_gettime(CLOCK_REALTIME, &ts_real);
    clock_gettime(CLOCK_MONOTONIC, &ts_mono);
    int64_t delta_real_mono = ((int64_t)ts_real.tv_sec * 1000000000LL + (int64_t)ts_real.tv_nsec) -
                              ((int64_t)ts_mono.tv_sec * 1000000000LL + (int64_t)ts_mono.tv_nsec);

    g_srv_spans = crear_servidor_unix(spans_sock, 16);
    if (g_srv_spans < 0) {
        perror("crear_servidor_unix spans.sock");
        if (alm_ok) {
        almacen_liberar(&alm);
    }
    for (int i = 0; i < n_lote; i++) free(lineas_lote[i]);
        free(lineas_lote);
        return 1;
    }

    pthread_t th_aceptar;
    if (pthread_create(&th_aceptar, NULL, hilo_aceptar_spans, NULL) != 0) {
        close(g_srv_spans);
        unlink(spans_sock);
        if (alm_ok) {
        almacen_liberar(&alm);
    }
    for (int i = 0; i < n_lote; i++) free(lineas_lote[i]);
        free(lineas_lote);
        return 1;
    }

    pid_t pid_srv_nuevo = -1;
    int pid_servicio = 0;
    const char *host = (cfg->backend_host != NULL) ? cfg->backend_host : "127.0.0.1";
    int puerto = (cfg->backend_puerto > 0) ? cfg->backend_puerto : 8000;

    if (cfg->destino == DEST_BACKEND) {
        if (cfg->lanzar_backend != NULL) {
            pid_srv_nuevo = lanzar_servicio_cmd(cfg->lanzar_backend, cfg->mostrar_salida);
            if (pid_srv_nuevo <= 0 || esperar_tcp_listo(host, puerto, 5000) != 0) {
                fprintf(stderr, "[tracelite] El backend lanzado no aceptó conexiones en %s:%d.\n", host, puerto);
                detener_servicio_nuevo(pid_srv_nuevo);
                return 1;
            }
            pid_servicio = (int)pid_srv_nuevo;
        } else if (cfg->backend_pid > 0) {
            pid_servicio = cfg->backend_pid;
        } else if (cfg->unidad != NULL) {
            pid_servicio = resolver_pid_unidad(cfg->unidad);
            if (pid_servicio == 0) {
                esperar_tcp_listo(host, puerto, 200);
                pid_servicio = resolver_pid_unidad(cfg->unidad);
            }
            if (pid_servicio <= 0) {
                fprintf(stderr, "[tracelite] Servicio %s inactivo o sin MainPID.\n", cfg->unidad);
                return 1;
            }
        } else {
            pid_servicio = resolver_pid_puerto_tcp(puerto);
            if (pid_servicio < 0 && esperar_tcp_listo(host, puerto, 50) == 0) {
                fprintf(stderr, "[tracelite] No se pudo resolver el PID del puerto %d (usa --backend-pid o --unidad).\n", puerto);
                return 1;
            }
        }
    } else if (cfg->destino == DEST_APP) {
        if (cfg->lanzar_app != NULL) {
            pid_srv_nuevo = lanzar_servicio_cmd(cfg->lanzar_app, cfg->mostrar_salida);
            if (pid_srv_nuevo <= 0) {
                fprintf(stderr, "[tracelite] No se pudo lanzar la app: %s\n", cfg->lanzar_app);
                return 1;
            }
            dormir_ms(500);
            pid_servicio = (int)pid_srv_nuevo;
        } else if (cfg->unidad != NULL) {
            pid_servicio = resolver_pid_unidad(cfg->unidad);
            if (pid_servicio <= 0) {
                fprintf(stderr, "[tracelite] Servicio %s inactivo o sin MainPID.\n", cfg->unidad);
                return 1;
            }
        } else {
            pid_servicio = resolver_pid_nombre_proceso(cfg->app_target);
            if (pid_servicio <= 0) {
                fprintf(stderr, "[tracelite] No se encontró el proceso de la app '%s'.\n",
                        cfg->app_target ? cfg->app_target : "");
                return 1;
            }
        }
    }

    /* 1. Lanzar Servicio B y Servicio A con fork() + execv() */
    char pid_srv_str[16], lb_str[16], puerto_str[16], fin_ord_str[4];
    snprintf(pid_srv_str, sizeof(pid_srv_str), "%d", pid_servicio);
    snprintf(lb_str, sizeof(lb_str), "%d", (cfg->destino != DEST_PROGRAMA && pid_servicio > 0) ? cfg->linea_base_s : 0);
    snprintf(puerto_str, sizeof(puerto_str), "%d", puerto);
    snprintf(fin_ord_str, sizeof(fin_ord_str), "%d", cfg->fin_por_orden ? 1 : 0);

    if (cfg->ventana_seg > 0) {
        char vbuf[16];
        snprintf(vbuf, sizeof(vbuf), "%d", cfg->ventana_seg);
        setenv("TRACELITE_VENTANA_SEG", vbuf, 1);
    } else {
        unsetenv("TRACELITE_VENTANA_SEG");
    }
    fflush(stdout);
    pid_t pid_b = fork();
    if (pid_b == 0) {
        const char *fstr = (cfg->fuente != NULL) ? cfg->fuente : "auto";
        char *args_b[10] = {
            ruta_b, b_sock, (char *)spans_sock, (char *)fstr, pid_srv_str, lb_str,
            (cfg->destino == DEST_BACKEND) ? puerto_str : "0",
            fin_ord_str, NULL, NULL
        };
        char plazo_b[16];
        snprintf(plazo_b, sizeof(plazo_b), "%d", cfg->plazo_s);
        args_b[8] = plazo_b;
        execv(ruta_b, args_b);
        perror("execv servicio_b");
        _exit(127);
    }

    char plazo_str[16], mostrar_str[4];
    snprintf(plazo_str, sizeof(plazo_str), "%d", cfg->plazo_s);
    snprintf(mostrar_str, sizeof(mostrar_str), "%d", cfg->mostrar_salida ? 1 : 0);

    printf("[tracelite] modo=%s tiempo=%s plazo=%d s\n",
           nombre_modo(cfg->modo), nombre_tiempo(cfg->tiempo_nivel), cfg->plazo_s);
    fflush(stdout);

    fflush(stdout);
    pid_t pid_a = fork();
    if (pid_a == 0) {
        const char *dest_str = (cfg->destino == DEST_BACKEND) ? "backend"
                             : ((cfg->destino == DEST_APP) ? "app" : "programa");
        char *args_a[11] = {
            ruta_a, a_sock, b_sock, (char *)spans_sock, plazo_str, mostrar_str,
            (char *)dest_str, (char *)host, puerto_str, pid_srv_str, NULL
        };
        execv(ruta_a, args_a);
        perror("execv servicio_a");
        _exit(127);
    }

    int a_fd = conectar_unix(a_sock, 40, 25);
    if (a_fd >= 0) {
        struct timeval tv;
        int extra_lb = (cfg->destino != DEST_PROGRAMA && pid_servicio > 0) ? cfg->linea_base_s : 0;
        tv.tv_sec = cfg->plazo_s + extra_lb + 5;
        tv.tv_usec = 0;
        setsockopt(a_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    char entradas_arr[32][128];
    int n_entradas = dividir_csv(cfg->entradas, entradas_arr, 32);
    char tamanos_arr[32][128];
    int n_tamanos = dividir_csv(cfg->tamanos ? cfg->tamanos : "1,2,4,8,16", tamanos_arr, 32);
    double pesos[32];
    for (int i = 0; i < 32; i++) {
        pesos[i] = (i < n_tamanos && atof(tamanos_arr[i]) > 0.0) ? atof(tamanos_arr[i]) : 1.0;
    }

    /* Calibración en estabilidad (no se guarda ni cuenta en el lote) */
    if (cfg->resumen && cfg->modo == MODO_ESTABILIDAD &&
        cfg->lote_path == NULL && n_entradas == 0 && a_fd >= 0 && !(*detener_flag)) {
        sol_t cal;
        memset(&cal, 0, sizeof(cal));
        cal.tipo = SOL_EJECUTAR;
        cal.solicitud = 0;
        construir_linea_solicitud(cfg, 0, NULL, cal.linea, sizeof(cal.linea));
        if (escribir_exacto(a_fd, &cal, sizeof(cal)) == 1) {
            sol_resp_t csr;
            memset(&csr, 0, sizeof(csr));
            if (leer_exacto(a_fd, &csr, sizeof(csr)) == 1) {
                int wms = 0;
                while (wms < 5000 && contar_spans_de_traza(csr.trace_id) < 2) {
                    dormir_ms(10);
                    wms += 10;
                }
                pthread_mutex_lock(&g_mtx);
                int w = 0;
                for (int k = 0; k < g_n_spans; k++) {
                    if (strcmp(g_spans[k].trace_id, csr.trace_id) != 0) {
                        g_spans[w++] = g_spans[k];
                    }
                }
                g_n_spans = w;
                pthread_mutex_unlock(&g_mtx);
            }
        }
    }

    int total_sol = (cfg->lote_path != NULL) ? n_lote : cfg->n_solicitudes;
    if (n_entradas > 0 && cfg->lote_path == NULL && total_sol > n_entradas) {
        total_sol = n_entradas;
    }
    int completadas = 0;
    int total_alertas = 0;
    char lote_tids[64][33];
    int n_lote_tids = 0;

    for (int i = 1; i <= total_sol && !(*detener_flag); i++) {
        if (a_fd < 0) break;

        sol_t sol;
        memset(&sol, 0, sizeof(sol));
        sol.tipo = SOL_EJECUTAR;
        sol.solicitud = i;
        if (cfg->lote_path != NULL) {
            snprintf(sol.linea, sizeof(sol.linea), "%s", lineas_lote[i - 1]);
        } else {
            const char *ent = (i - 1 < n_entradas) ? entradas_arr[i - 1] : NULL;
            construir_linea_solicitud(cfg, i, ent, sol.linea, sizeof(sol.linea));
        }

        if (escribir_exacto(a_fd, &sol, sizeof(sol)) != 1) {
            break;
        }

        int64_t t_sol_ini = ahora_ns();
        int ult_seg = -1;
        while (!(*detener_flag)) {
            int seg = (int)((ahora_ns() - t_sol_ini) / 1000000000LL);
            if (seg != ult_seg) {
                ult_seg = seg;
                int meta_s = (cfg->ventana_seg > 0) ? cfg->ventana_seg : cfg->plazo_s;
                if (total_sol > 1 || cfg->resumen || cfg->ventana_seg > 0) {
                    printf("\r[tracelite] Solicitud %d / %d: %2d / %d s...",
                           i, total_sol, seg, meta_s);
                    fflush(stdout);
                }
            }
            struct pollfd pfd;
            pfd.fd = a_fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            int pr = poll(&pfd, 1, 200);
            if (pr > 0) {
                break;
            }
            if (pr < 0 && errno != EINTR) {
                break;
            }
        }

        sol_resp_t sr;
        memset(&sr, 0, sizeof(sr));
        if (leer_exacto(a_fd, &sr, sizeof(sr)) != 1) {
            break;
        }

        /* Esperar a que ambos spans estén en Collector Lite (máximo 5 s) */
        int espera_ms = 0;
        while (espera_ms < 5000 && contar_spans_de_traza(sr.trace_id) < 2) {
            if (sr.estado != 0 && espera_ms >= 200) {
                break;
            }
            dormir_ms(10);
            espera_ms += 10;
        }

        if (sr.trace_id[0] != '\0') {
            if (n_lote_tids < 64) {
                snprintf(lote_tids[n_lote_tids++], 33, "%s", sr.trace_id);
            }
            if (cfg->modo == MODO_RESPUESTA || cfg->modo == MODO_FALLO || !cfg->resumen) {
                total_alertas += evaluar_y_alertar_solicitud(cfg, &alm, alm_ok, sr.trace_id);
            }
        }
        completadas++;
    }
    if ((total_sol > 1 || cfg->resumen) && completadas > 0) {
        printf("\n");
    }

    if (cfg->resumen && cfg->modo == MODO_ESTABILIDAD) {
        for (int t = 0; t < n_lote_tids; t++) {
            total_alertas += evaluar_y_alertar_solicitud(cfg, &alm, alm_ok, lote_tids[t]);
        }
    } else if (cfg->resumen && cfg->modo == MODO_ESCALADO) {
        total_alertas += evaluar_escalado_lote(pesos, 32, lote_tids, n_lote_tids);
    }

    if (alm_ok) {
        for (int t = 0; t < n_lote_tids; t++) {
            int n_traza = contar_spans_de_traza(lote_tids[t]);
            pthread_mutex_lock(&g_mtx);
            for (int k = 0; k < g_n_spans; k++) {
                if (strcmp(g_spans[k].trace_id, lote_tids[t]) == 0) {
                    if (n_traza < 2 && g_spans[k].msg[0] == '\0') {
                        snprintf(g_spans[k].msg, sizeof(g_spans[k].msg), "traza_incompleta");
                    }
                    almacen_meta_t meta;
                    memset(&meta, 0, sizeof(meta));
                    snprintf(meta.tipo, sizeof(meta.tipo), "solicitud");
                    snprintf(meta.modo, sizeof(meta.modo), "%s", nombre_modo(cfg->modo));
                    snprintf(meta.destino, sizeof(meta.destino), "%s", nombre_destino(cfg->destino));
                    snprintf(meta.tiempo, sizeof(meta.tiempo), "%s", nombre_tiempo(cfg->tiempo_nivel));
                    meta.plazo_s = cfg->plazo_s;
                    meta.solicitud = t + 1;
                    meta.ini_real_ns = g_spans[k].ini + delta_real_mono;
                    meta.fin_real_ns = g_spans[k].fin + delta_real_mono;
                    almacen_guardar_span(&alm, &g_spans[k], &meta);
                }
            }
            pthread_mutex_unlock(&g_mtx);
        }
        almacen_aplicar_retencion(&alm, cfg->ret_dias, cfg->ret_seg);
    }

    if (a_fd >= 0) {
        sol_t fin;
        memset(&fin, 0, sizeof(fin));
        fin.tipo = SOL_FIN;
        escribir_exacto(a_fd, &fin, sizeof(fin));
        close(a_fd);
    }

    esperar_pid_seguro(pid_a);
    esperar_pid_seguro(pid_b);
    detener_servicio_nuevo(pid_srv_nuevo);

    pthread_mutex_lock(&g_mtx);
    g_cerrar_srv = 1;
    shutdown(g_srv_spans, SHUT_RDWR);
    pthread_mutex_unlock(&g_mtx);

    pthread_join(th_aceptar, NULL);
    close(g_srv_spans);
    unlink(a_sock);
    unlink(b_sock);
    unlink(spans_sock);

    imprimir_trazas_spans(g_spans, g_n_spans);

    int total_err = 0;
    for (int i = 0; i < g_n_spans; i++) {
        if (g_spans[i].estado != 0) {
            total_err++;
        }
    }
    printf("\n-- Resumen: solicitudes=%d spans=%d alertas=%d errores=%d --\n",
           completadas, g_n_spans, total_alertas, total_err);

    if (alm_ok) {
        almacen_liberar(&alm);
    }
    for (int i = 0; i < n_lote; i++) free(lineas_lote[i]);
    free(lineas_lote);
    free(g_spans);
    g_spans = NULL;
    g_n_spans = 0;
    g_cap_spans = 0;
    return (total_err > 0 && completadas == 0) ? 1 : 0;
}
