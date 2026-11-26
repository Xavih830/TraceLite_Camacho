#include "common.h"
#include "fuente.h"
#include "protocolo.h"

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Servicio B (observa): mide el impacto del SO por solicitud y reporta su span */

extern const fuente_t fuente_proc;
#ifdef HAVE_EBPF
extern const fuente_t fuente_ebpf;
#endif

static int verificar_peer_uid(int fd) {
    struct ucred cred;
    socklen_t len = sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
        return -1;
    }
    return (cred.uid == getuid()) ? 0 : -1;
}

typedef struct {
    int     pid;
    int     ppid;
    char    comm[32];
    char    span_id[17];
    char    parent_span[17];
    int64_t ini_ns;
    int64_t fin_ns;
    unsigned long long ticks_ini;
    unsigned long long ticks_ult;
    int     visto;
    int     finalizado;
} proc_vivo_t;

typedef struct {
    proc_vivo_t  arbol[64];
    int          n_arb;
    int          pid_raiz;
    char         sb_span_id[17];
    volatile int corriendo;
    int          activo_hilo;
    pthread_t    th;
} muestreo_app_t;

static void muestrear_una_vez(muestreo_app_t *m) {
    DIR *dir = opendir("/proc");
    if (dir == NULL) return;

    proc_vivo_t lista[512];
    int n_lista = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL && n_lista < 512) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        int pid = atoi(de->d_name);
        if (pid <= 0) continue;
        char ruta[64];
        snprintf(ruta, sizeof(ruta), "/proc/%d/stat", pid);
        FILE *f = fopen(ruta, "r");
        if (f == NULL) continue;
        char buf[512];
        size_t nr = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        if (nr == 0) continue;
        buf[nr] = '\0';
        char *lp = strchr(buf, '(');
        char *rp = strrchr(buf, ')');
        if (lp == NULL || rp == NULL || rp <= lp) continue;
        size_t clen = (size_t)(rp - lp - 1);
        if (clen >= sizeof(lista[n_lista].comm)) clen = sizeof(lista[n_lista].comm) - 1;
        memcpy(lista[n_lista].comm, lp + 1, clen);
        lista[n_lista].comm[clen] = '\0';
        char est = 0;
        int ppid = 0;
        unsigned long long ut = 0, st = 0;
        int ntok = sscanf(rp + 2,
                          "%c %d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu",
                          &est, &ppid, &ut, &st);
        if (ntok >= 2) {
            lista[n_lista].pid = pid;
            lista[n_lista].ppid = ppid;
            lista[n_lista].ticks_ult = (ntok == 4) ? (ut + st) : 0ULL;
            n_lista++;
        }
    }
    closedir(dir);

    /* Recolectar subárbol actual desde pid_raiz */
    int sub_idx[64];
    int n_sub = 0;
    for (int i = 0; i < n_lista; i++) {
        if (lista[i].pid == m->pid_raiz) {
            sub_idx[n_sub++] = i;
            break;
        }
    }
    for (int head = 0; head < n_sub && n_sub < 64; head++) {
        int p_pid = lista[sub_idx[head]].pid;
        for (int i = 0; i < n_lista && n_sub < 64; i++) {
            if (lista[i].ppid == p_pid) {
                sub_idx[n_sub++] = i;
            }
        }
    }

    for (int i = 0; i < m->n_arb; i++) m->arbol[i].visto = 0;
    int64_t t_now = ahora_ns();

    for (int s = 0; s < n_sub; s++) {
        const proc_vivo_t *p = &lista[sub_idx[s]];
        int idx = -1;
        for (int j = 0; j < m->n_arb; j++) {
            if (!m->arbol[j].finalizado && m->arbol[j].pid == p->pid) {
                idx = j;
                break;
            }
        }
        if (idx >= 0) {
            m->arbol[idx].visto = 1;
            m->arbol[idx].fin_ns = t_now;
            m->arbol[idx].ticks_ult = p->ticks_ult;
        } else if (m->n_arb < 64) {
            proc_vivo_t *dst = &m->arbol[m->n_arb++];
            memset(dst, 0, sizeof(*dst));
            dst->pid = p->pid;
            dst->ppid = p->ppid;
            snprintf(dst->comm, sizeof(dst->comm), "%s", p->comm);
            id_hex(dst->span_id, 8);
            snprintf(dst->parent_span, sizeof(dst->parent_span), "(raiz)");
            if (m->sb_span_id[0] != '\0') {
                memcpy(dst->parent_span, m->sb_span_id, sizeof(dst->parent_span));
                dst->parent_span[sizeof(dst->parent_span) - 1] = '\0';
            }
            for (int k = m->n_arb - 2; k >= 0; k--) {
                if (!m->arbol[k].finalizado && m->arbol[k].pid == p->ppid) {
                    memcpy(dst->parent_span, m->arbol[k].span_id, sizeof(dst->parent_span));
                    dst->parent_span[sizeof(dst->parent_span) - 1] = '\0';
                    break;
                }
            }
            dst->ini_ns = t_now;
            dst->fin_ns = t_now;
            dst->ticks_ini = p->ticks_ult;
            dst->ticks_ult = p->ticks_ult;
            dst->visto = 1;
        }
    }
    for (int i = 0; i < m->n_arb; i++) {
        if (!m->arbol[i].finalizado && !m->arbol[i].visto) {
            m->arbol[i].finalizado = 1;
            m->arbol[i].fin_ns = t_now;
        }
    }
}

static void *hilo_muestreo_app(void *arg) {
    muestreo_app_t *m = (muestreo_app_t *)arg;
    while (m->corriendo) {
        muestrear_una_vez(m);
        dormir_ms(80);
    }
    return NULL;
}

static void emitir_arbol_app_muestreado(int spans_fd, const span_t *sb,
                                        const muestreo_app_t *m) {
    if (spans_fd < 0 || m->n_arb <= 0) return;
    long clk_tck = sysconf(_SC_CLK_TCK);
    if (clk_tck <= 0) clk_tck = 100;
    for (int i = 0; i < m->n_arb; i++) {
        const proc_vivo_t *r = &m->arbol[i];
        span_t s;
        memset(&s, 0, sizeof(s));
        snprintf(s.trace_id, sizeof(s.trace_id), "%s", sb->trace_id);
        snprintf(s.span_id, sizeof(s.span_id), "%s", r->span_id);
        snprintf(s.parent, sizeof(s.parent), "%s", r->parent_span);
        snprintf(s.servicio, sizeof(s.servicio), "%s", r->comm);
        snprintf(s.operacion, sizeof(s.operacion), "pid:%d", r->pid);
        s.ini = (r->ini_ns >= sb->ini) ? r->ini_ns : sb->ini;
        s.fin = (r->fin_ns > s.ini) ? r->fin_ns : (s.ini + 1000000LL);
        if (s.fin > sb->fin) s.fin = sb->fin;
        s.dur = (double)(s.fin - s.ini) / 1000000.0;
        s.pid = r->pid;
        s.n_procs = 1;
        if (r->ticks_ult >= r->ticks_ini) {
            s.cpu_ms = (double)(r->ticks_ult - r->ticks_ini) * 1000.0 / (double)clk_tck;
        }
        snprintf(s.atribucion, sizeof(s.atribucion), "%s", sb->atribucion);
        escribir_exacto(spans_fd, &s, sizeof(span_t));
    }
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Uso: %s <b_sock> <spans_sock> [fuente] [pid_servicio] [linea_base_s]\n", argv[0]);
        return 1;
    }

    const char *b_sock = argv[1];
    const char *spans_sock = argv[2];
    const char *modo_fuente = (argc > 3) ? argv[3] : "auto";
    int pid_servicio = (argc > 4) ? atoi(argv[4]) : 0;
    int linea_base_s = (argc > 5) ? atoi(argv[5]) : 0;
    int puerto_backend = (argc > 6) ? atoi(argv[6]) : 0;
    int fin_por_orden = (argc > 7) ? atoi(argv[7]) : 0;
    int plazo_s = (argc > 8) ? atoi(argv[8]) : 10;
    if (plazo_s <= 0) plazo_s = 10;

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, SIG_IGN);

    const fuente_t *fuente = NULL;
    if (strcmp(modo_fuente, "proc") == 0) {
        fuente = &fuente_proc;
        if (fuente->abrir() != 0) {
            fprintf(stderr, "[servicio_b] No se pudo abrir /proc.\n");
            return 1;
        }
    } else {
#ifdef HAVE_EBPF
        if (fuente_ebpf.abrir() == 0) {
            fuente = &fuente_ebpf;
        } else if (strcmp(modo_fuente, "ebpf") == 0) {
            fprintf(stderr, "[servicio_b] Error: --fuente ebpf fallo al cargar eBPF (verifica cap_bpf,cap_perfmon).\n");
            return 1;
        } else {
            fprintf(stderr, "[servicio_b] Aviso: eBPF no disponible o sin permisos; usando /proc.\n");
            fuente = &fuente_proc;
            if (fuente->abrir() != 0) return 1;
        }
#else
        if (strcmp(modo_fuente, "ebpf") == 0) {
            fprintf(stderr, "[servicio_b] Error: binario compilado sin soporte eBPF (ejecuta 'make ebpf').\n");
            return 1;
        }
        fprintf(stderr, "[servicio_b] Aviso: compilado sin eBPF; usando /proc.\n");
        fuente = &fuente_proc;
        if (fuente->abrir() != 0) return 1;
#endif
    }

    int srv_fd = crear_servidor_unix(b_sock, 4);
    if (srv_fd < 0) {
        perror("crear_servidor_unix b.sock");
        fuente->cerrar();
        return 1;
    }

    int a_fd = aceptar_unix(srv_fd, 65000);
    if (a_fd < 0 || verificar_peer_uid(a_fd) != 0) {
        if (a_fd >= 0) close(a_fd);
        close(srv_fd);
        unlink(b_sock);
        fuente->cerrar();
        return 1;
    }

    int spans_fd = conectar_unix(spans_sock, 20, 25);

    double tasa_base = 0.0;
    int tiene_base = 0;
    if (pid_servicio > 0 && linea_base_s > 0) {
        if (fuente->iniciar(0, pid_servicio, 0) == 0) {
            dormir_ms(linea_base_s * 1000);
            fuente->detener();
            medida_t mb;
            memset(&mb, 0, sizeof(mb));
            fuente->leer(&mb);
            tasa_base = mb.cpu_ms / ((double)linea_base_s * 1000.0);
            tiene_base = 1;
            if (tasa_base > 0.25) {
                fprintf(stderr, "[servicio_b] Aviso: linea base alta (%.1f%% CPU); atribucion sera pobre.\n",
                        tasa_base * 100.0);
            }
        }
    }

    span_t sb;
    int activo = 0;
    int pid_obj = 0;
    const fuente_t *fuente_act = fuente;
    muestreo_app_t muestreo;
    memset(&muestreo, 0, sizeof(muestreo));

    ctx_t ctx;
    while (leer_exacto(a_fd, &ctx, sizeof(ctx)) == 1) {
        if (ctx.tipo == TL_MSG_FIN) {
            break;
        }
        if (ctx.tipo == TL_MSG_INICIAR) {
            pid_obj = (ctx.pid_programa > 0) ? ctx.pid_programa : ctx.pid_app;
            span_ini(&sb, ctx.trace_id, ctx.parent_span, "servicio_b", ctx.operacion);
            sb.pid = pid_obj;
            if (ctx.puerto_cliente > 0 && puerto_backend > 0 && fuente->iniciar_conn != NULL) {
                fuente_act = fuente;
                fuente_act->iniciar_conn(ctx.puerto_cliente, puerto_backend, ctx.solicitud);
            } else if (ctx.pid_programa == 0 && ctx.pid_app > 0) {
                fuente_act = &fuente_proc;
                fuente_act->iniciar(0, ctx.pid_app, ctx.solicitud);
                memset(&muestreo, 0, sizeof(muestreo));
                muestreo.pid_raiz = ctx.pid_app;
                snprintf(muestreo.sb_span_id, sizeof(muestreo.sb_span_id), "%s", sb.span_id);
                muestreo.corriendo = 1;
                if (pthread_create(&muestreo.th, NULL, hilo_muestreo_app, &muestreo) == 0) {
                    muestreo.activo_hilo = 1;
                }
            } else {
                fuente_act = fuente;
                fuente_act->iniciar(ctx.pid_programa, ctx.pid_app, ctx.solicitud);
            }
            activo = 1;

            resp_t r;
            memset(&r, 0, sizeof(r));
            r.tipo = TL_MSG_LISTO;
            r.solicitud = ctx.solicitud;
            r.fin_ns = ahora_ns();
            escribir_exacto(a_fd, &r, sizeof(r));
        } else if (ctx.tipo == TL_MSG_TERMINAR && activo) {
            medida_t m;
            memset(&m, 0, sizeof(m));
            int sin_reposo = 0;
            if (ctx.pid_programa > 0) {
                int espera = 0;
                while (espera < 2000) {
                    fuente_act->leer(&m);
                    if (m.n_vivos == 0) break;
                    dormir_ms(20);
                    espera += 20;
                }
            } else if (ctx.pid_app > 0 && puerto_backend == 0 && !fin_por_orden) {
                /* Esperar 3 ventanas seguidas de 100 ms en reposo */
                double umbral_cpu = tasa_base * 100.0 + 2.0;
                int reposo_seguido = 0;
                int espera_ms = 0;
                double cpu_prev = 0.0;
                fuente_act->leer(&m);
                cpu_prev = m.cpu_ms;
                while (espera_ms < plazo_s * 1000) {
                    dormir_ms(100);
                    espera_ms += 100;
                    fuente_act->leer(&m);
                    double delta = (m.cpu_ms > cpu_prev) ? (m.cpu_ms - cpu_prev) : 0.0;
                    cpu_prev = m.cpu_ms;
                    if (delta <= umbral_cpu) {
                        reposo_seguido++;
                        if (reposo_seguido >= 3) break;
                    } else {
                        reposo_seguido = 0;
                    }
                }
                if (reposo_seguido < 3) sin_reposo = 1;
            }
            if (ctx.pid_programa == 0 && ctx.pid_app > 0 && puerto_backend == 0) {
                const char *vs = getenv("TRACELITE_VENTANA_SEG");
                int vseg = (vs != NULL) ? atoi(vs) : 0;
                if (vseg > 0) {
                    int64_t meta_ns = sb.ini + (int64_t)vseg * 1000000000LL;
                    while (ahora_ns() < meta_ns) {
                        dormir_ms(100);
                    }
                }
            }
            fuente_act->detener();
            fuente_act->leer(&m);
            if (muestreo.activo_hilo) {
                muestreo.corriendo = 0;
                pthread_join(muestreo.th, NULL);
                muestreo.activo_hilo = 0;
            }
            activo = 0;

            int estado_b = (ctx.estado_salida != 0) ? 1 : 0;
            char msg_b[64] = "";
            char anom_b[24] = "";
            if (estado_b != 0) {
                snprintf(anom_b, sizeof(anom_b), "error_proceso");
                if (ctx.estado_salida == -2) {
                    snprintf(msg_b, sizeof(msg_b), "plazo_vencido");
                } else if (ctx.estado_salida == -1) {
                    snprintf(msg_b, sizeof(msg_b), "conexion_fallida");
                } else if (ctx.estado_salida >= 100) {
                    snprintf(msg_b, sizeof(msg_b), "http_%d", ctx.estado_salida);
                } else {
                    snprintf(msg_b, sizeof(msg_b), "exit_%d", ctx.estado_salida);
                }
            } else if (sin_reposo) {
                snprintf(msg_b, sizeof(msg_b), "sin_reposo");
            } else if (m.perdidos > 0) {
                snprintf(msg_b, sizeof(msg_b), "perdidos=%lu", m.perdidos);
            } else if (ctx.pid_programa > 0 && m.n_vivos > 0) {
                snprintf(msg_b, sizeof(msg_b), "procesos_activos_%d", m.n_vivos);
            }

            span_fin(&sb, estado_b, msg_b, anom_b);
            double cpu_final = m.cpu_ms;
            if (tiene_base) {
                double fondo_ms = tasa_base * sb.dur;
                cpu_final = (m.cpu_ms > fondo_ms) ? (m.cpu_ms - fondo_ms) : 0.0;
                if (sb.msg[0] == '\0') {
                    snprintf(sb.msg, sizeof(sb.msg), "fondo=%.1fms", fondo_ms);
                } else {
                    char tmp_msg[64];
                    snprintf(tmp_msg, sizeof(tmp_msg), "%.38s fondo=%.1fms", sb.msg, fondo_ms);
                    snprintf(sb.msg, sizeof(sb.msg), "%s", tmp_msg);
                }
            }
            sb.pid = pid_obj;
            sb.cpu_ms = cpu_final;
            sb.n_procs = m.n_procs;
            snprintf(sb.atribucion, sizeof(sb.atribucion), "%s", m.atribucion);

            if (spans_fd >= 0) {
                escribir_exacto(spans_fd, &sb, sizeof(span_t));
                if (ctx.pid_programa == 0 && ctx.pid_app > 0 && puerto_backend == 0) {
                    emitir_arbol_app_muestreado(spans_fd, &sb, &muestreo);
                }
            }

            resp_t r;
            memset(&r, 0, sizeof(r));
            r.tipo = TL_MSG_RESULTADO;
            r.solicitud = ctx.solicitud;
            r.fin_ns = sb.fin;
            r.estado = estado_b;
            snprintf(r.msg, sizeof(r.msg), "%s", msg_b);
            escribir_exacto(a_fd, &r, sizeof(r));
        }
    }

    if (activo) {
        fuente->detener();
    }
    if (spans_fd >= 0) {
        close(spans_fd);
    }
    close(a_fd);
    close(srv_fd);
    unlink(b_sock);
    fuente->cerrar();
    return 0;
}
