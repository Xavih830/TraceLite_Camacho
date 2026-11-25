#include "common.h"
#include "fuente.h"
#include "protocolo.h"

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
            fuente_act->detener();
            fuente_act->leer(&m);
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
