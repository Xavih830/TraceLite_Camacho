#include "common.h"
#include "protocolo.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

/* Servicio A (entrega): crea contexto, ejecuta programa o petición HTTP y coordina con B */

static void extraer_op_http(const char *ruta, char *out, size_t out_sz) {
    char tmp[48];
    snprintf(tmp, sizeof(tmp), "%.47s", (ruta && ruta[0]) ? ruta : "/");
    char *q = strchr(tmp, '?');
    if (q != NULL) *q = '\0';
    snprintf(out, out_sz, "GET %.40s", tmp);
}

static int ejecutar_http_backend(int b_fd, int spans_fd, int tl_fd, const sol_t *sol,
                                 const char *host, int puerto, int pid_servicio, int plazo_s) {
    char op[48];
    extraer_op_http(sol->linea, op, sizeof(op));

    span_t sa;
    span_ini(&sa, NULL, NULL, "servicio_a", op);

    int sfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sfd >= 0) {
        struct timeval tv;
        tv.tv_sec = plazo_s;
        tv.tv_usec = 0;
        setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)puerto);
    inet_pton(AF_INET, host, &addr.sin_addr);

    int conectado = (sfd >= 0 && connect(sfd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    int puerto_cli = 0;
    if (conectado) {
        struct sockaddr_in local;
        socklen_t lsz = sizeof(local);
        if (getsockname(sfd, (struct sockaddr *)&local, &lsz) == 0) {
            puerto_cli = (int)ntohs(local.sin_port);
        }
    }

    int b_caido = (b_fd < 0);
    if (!b_caido) {
        ctx_t ini_b;
        memset(&ini_b, 0, sizeof(ini_b));
        ini_b.tipo = TL_MSG_INICIAR;
        ini_b.solicitud = sol->solicitud;
        snprintf(ini_b.trace_id, sizeof(ini_b.trace_id), "%s", sa.trace_id);
        snprintf(ini_b.parent_span, sizeof(ini_b.parent_span), "%s", sa.span_id);
        ini_b.pid_app = pid_servicio;
        ini_b.puerto_cliente = puerto_cli;
        snprintf(ini_b.operacion, sizeof(ini_b.operacion), "%s", op);

        resp_t listo;
        if (escribir_exacto(b_fd, &ini_b, sizeof(ini_b)) != 1 ||
            leer_exacto(b_fd, &listo, sizeof(listo)) != 1 ||
            listo.tipo != TL_MSG_LISTO) {
            b_caido = 1;
        }
    }

    int http_code = 0;
    long body_bytes = 0;
    int timeout_hit = 0;

    if (conectado) {
        char req[1024];
        int rlen = snprintf(req, sizeof(req),
                            "GET %s HTTP/1.0\r\nHost: %s\r\n"
                            "traceparent: 00-%s-%s-01\r\nConnection: close\r\n\r\n",
                            sol->linea[0] ? sol->linea : "/", host,
                            sa.trace_id, sa.span_id);
        if (rlen > 0 && escribir_exacto(sfd, req, (size_t)rlen) == 1) {
            char buf[4096];
            char hdr[1024];
            size_t hlen = 0;
            int en_cuerpo = 0;
            while (1) {
                ssize_t n = recv(sfd, buf, sizeof(buf), 0);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) timeout_hit = 1;
                    break;
                }
                if (n == 0) break;

                if (!en_cuerpo) {
                    size_t copy = ((size_t)n < sizeof(hdr) - 1 - hlen) ? (size_t)n : (sizeof(hdr) - 1 - hlen);
                    memcpy(hdr + hlen, buf, copy);
                    hlen += copy;
                    hdr[hlen] = '\0';
                    char *sep = strstr(hdr, "\r\n\r\n");
                    if (sep != NULL) {
                        en_cuerpo = 1;
                        sscanf(hdr, "HTTP/%*s %d", &http_code);
                        size_t hdr_tot = (size_t)(sep - hdr) + 4;
                        if (hlen > hdr_tot) body_bytes += (long)(hlen - hdr_tot);
                    }
                } else {
                    body_bytes += (long)n;
                }
            }
            if (!en_cuerpo && hlen > 0) {
                sscanf(hdr, "HTTP/%*s %d", &http_code);
            }
        }
    }
    if (sfd >= 0) close(sfd);
    int64_t fin_http_ns = ahora_ns();

    int estado_salida = 0;
    if (!conectado) estado_salida = -1;
    else if (timeout_hit) estado_salida = -2;
    else if (http_code == 0 || http_code >= 400) estado_salida = (http_code > 0) ? http_code : -1;

    resp_t resp_b;
    memset(&resp_b, 0, sizeof(resp_b));
    if (!b_caido) {
        ctx_t term_b;
        memset(&term_b, 0, sizeof(term_b));
        term_b.tipo = TL_MSG_TERMINAR;
        term_b.solicitud = sol->solicitud;
        snprintf(term_b.trace_id, sizeof(term_b.trace_id), "%s", sa.trace_id);
        snprintf(term_b.parent_span, sizeof(term_b.parent_span), "%s", sa.span_id);
        term_b.pid_app = pid_servicio;
        term_b.estado_salida = estado_salida;
        snprintf(term_b.operacion, sizeof(term_b.operacion), "%s", op);

        if (escribir_exacto(b_fd, &term_b, sizeof(term_b)) != 1 ||
            leer_exacto(b_fd, &resp_b, sizeof(resp_b)) != 1 ||
            resp_b.tipo != TL_MSG_RESULTADO) {
            b_caido = 1;
        }
    }

    int estado_a = (estado_salida != 0 || b_caido) ? 1 : 0;
    char msg_a[64], anom_a[24] = "";
    if (b_caido) {
        snprintf(msg_a, sizeof(msg_a), "servicio_b_caido");
        snprintf(anom_a, sizeof(anom_a), "error_proceso");
    } else if (!conectado) {
        snprintf(msg_a, sizeof(msg_a), "conexion_rechazada");
        snprintf(anom_a, sizeof(anom_a), "error_proceso");
    } else if (timeout_hit) {
        snprintf(msg_a, sizeof(msg_a), "plazo_vencido");
        snprintf(anom_a, sizeof(anom_a), "error_proceso");
    } else if (http_code >= 400) {
        snprintf(msg_a, sizeof(msg_a), "http_%d", http_code);
        snprintf(anom_a, sizeof(anom_a), "error_proceso");
    } else {
        snprintf(msg_a, sizeof(msg_a), "%ldB", body_bytes);
    }

    span_fin(&sa, estado_a, msg_a, anom_a);
    sa.fin = (!b_caido && resp_b.fin_ns > fin_http_ns) ? resp_b.fin_ns : fin_http_ns;
    if (sa.fin < sa.ini) sa.fin = sa.ini;
    sa.dur = (double)(sa.fin - sa.ini) / 1000000.0;

    if (spans_fd >= 0) escribir_exacto(spans_fd, &sa, sizeof(span_t));

    sol_resp_t sr;
    memset(&sr, 0, sizeof(sr));
    sr.tipo = TL_MSG_RESULTADO;
    sr.solicitud = sol->solicitud;
    sr.estado = estado_a;
    snprintf(sr.trace_id, sizeof(sr.trace_id), "%s", sa.trace_id);
    escribir_exacto(tl_fd, &sr, sizeof(sr));
    return 0;
}

static int dividir_argumentos(char *buf, char *argv_out[], int max_args) {
    int argc = 0;
    char *p = buf;
    while (*p != '\0' && argc < max_args - 1) {
        while (*p == ' ' || *p == '\t' || *p == '\n') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        argv_out[argc++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n') {
            p++;
        }
        if (*p != '\0') {
            *p++ = '\0';
        }
    }
    argv_out[argc] = NULL;
    return argc;
}

static int esperar_con_plazo(pid_t pid, int plazo_s, int *estado_out, int64_t *fin_ns) {
    int64_t limite_ns = (int64_t)plazo_s * 1000000000LL;
    int64_t ini_ns = ahora_ns();
    int st = 0;

    while (1) {
        pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) {
            *fin_ns = ahora_ns();
            if (WIFEXITED(st)) {
                *estado_out = WEXITSTATUS(st);
            } else if (WIFSIGNALED(st)) {
                *estado_out = 128 + WTERMSIG(st);
            } else {
                *estado_out = 1;
            }
            return 0;
        }
        if (w < 0 && errno != EINTR) {
            *fin_ns = ahora_ns();
            *estado_out = 1;
            return 0;
        }
        if (plazo_s > 0 && (ahora_ns() - ini_ns) >= limite_ns) {
            kill(-pid, SIGTERM);
            int espera = 0;
            while (espera < 2000) {
                w = waitpid(pid, &st, WNOHANG);
                if (w == pid) {
                    *fin_ns = ahora_ns();
                    *estado_out = -2;
                    return 1;
                }
                dormir_ms(20);
                espera += 20;
            }
            kill(-pid, SIGKILL);
            while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
            }
            *fin_ns = ahora_ns();
            *estado_out = -2;
            return 1;
        }
        dormir_ms(10);
    }
}

int main(int argc, char *argv[]) {
    if (argc < 6) {
        fprintf(stderr, "Uso: %s <a_sock> <b_sock> <spans_sock> <plazo_s> <mostrar_salida> [backend|programa] [host] [puerto] [pid_srv]\n", argv[0]);
        return 1;
    }

    const char *a_sock = argv[1];
    const char *b_sock = argv[2];
    const char *spans_sock = argv[3];
    int plazo_s = atoi(argv[4]);
    int mostrar_salida = atoi(argv[5]);
    int es_backend = (argc > 6 && strcmp(argv[6], "backend") == 0);
    int es_app = (argc > 6 && strcmp(argv[6], "app") == 0);
    const char *host = (argc > 7) ? argv[7] : "127.0.0.1";
    int puerto = (argc > 8) ? atoi(argv[8]) : 8000;
    int pid_servicio = (argc > 9) ? atoi(argv[9]) : 0;
    if (plazo_s <= 0) plazo_s = 10;

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, SIG_IGN);

    int srv_fd = crear_servidor_unix(a_sock, 4);
    if (srv_fd < 0) {
        perror("crear_servidor_unix a.sock");
        return 1;
    }

    int tl_fd = aceptar_unix(srv_fd, 65000);
    if (tl_fd < 0) {
        close(srv_fd);
        unlink(a_sock);
        return 1;
    }

    int b_fd = conectar_unix(b_sock, 40, 25);
    if (b_fd >= 0) {
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(b_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    int spans_fd = conectar_unix(spans_sock, 20, 25);

    sol_t sol;
    while (leer_exacto(tl_fd, &sol, sizeof(sol)) == 1) {
        if (sol.tipo == SOL_FIN) {
            if (b_fd >= 0) {
                ctx_t fin_b;
                memset(&fin_b, 0, sizeof(fin_b));
                fin_b.tipo = TL_MSG_FIN;
                escribir_exacto(b_fd, &fin_b, sizeof(fin_b));
            }
            break;
        }

        if (sol.tipo != SOL_EJECUTAR) {
            continue;
        }

        if (es_backend) {
            ejecutar_http_backend(b_fd, spans_fd, tl_fd, &sol, host, puerto, pid_servicio, plazo_s);
            continue;
        }

        char linea_copia[512];
        snprintf(linea_copia, sizeof(linea_copia), "%s", sol.linea);
        char *argv_cmd[64];
        if (dividir_argumentos(linea_copia, argv_cmd, 64) == 0) {
            continue;
        }

        char op[48];
        nombre_base_cmd(sol.linea, op, sizeof(op));

        span_t sa;
        span_ini(&sa, NULL, NULL, "servicio_a", op);

        int barrera[2];
        if (pipe2(barrera, O_CLOEXEC) != 0) {
            span_fin(&sa, 1, "pipe_fallo", "error_proceso");
            if (spans_fd >= 0) escribir_exacto(spans_fd, &sa, sizeof(span_t));
            continue;
        }

        fflush(stdout);
        fflush(stderr);
        pid_t pid_prog = fork();
        if (pid_prog < 0) {
            close(barrera[0]);
            close(barrera[1]);
            span_fin(&sa, 1, "fork_fallo", "error_proceso");
            if (spans_fd >= 0) escribir_exacto(spans_fd, &sa, sizeof(span_t));
            sol_resp_t sr;
            memset(&sr, 0, sizeof(sr));
            sr.tipo = TL_MSG_RESULTADO;
            sr.solicitud = sol.solicitud;
            sr.estado = 1;
            snprintf(sr.trace_id, sizeof(sr.trace_id), "%s", sa.trace_id);
            escribir_exacto(tl_fd, &sr, sizeof(sr));
            continue;
        }

        if (pid_prog == 0) {
            setpgid(0, 0);
            signal(SIGPIPE, SIG_DFL);
            signal(SIGINT, SIG_DFL);
            close(barrera[1]);
            char byte = 0;
            ssize_t r = read(barrera[0], &byte, 1);
            (void)r;
            close(barrera[0]);

            if (!mostrar_salida) {
                int devnull = open("/dev/null", O_WRONLY);
                if (devnull >= 0) {
                    dup2(devnull, STDOUT_FILENO);
                    dup2(devnull, STDERR_FILENO);
                    close(devnull);
                }
            }
            execvp(argv_cmd[0], argv_cmd);
            _exit(127);
        }

        setpgid(pid_prog, pid_prog);
        close(barrera[0]);

        int b_caido = (b_fd < 0);
        if (!b_caido) {
            ctx_t ini_b;
            memset(&ini_b, 0, sizeof(ini_b));
            ini_b.tipo = TL_MSG_INICIAR;
            ini_b.solicitud = sol.solicitud;
            snprintf(ini_b.trace_id, sizeof(ini_b.trace_id), "%s", sa.trace_id);
            snprintf(ini_b.parent_span, sizeof(ini_b.parent_span), "%s", sa.span_id);
            ini_b.pid_programa = es_app ? 0 : (int)pid_prog;
            ini_b.pid_app = es_app ? pid_servicio : 0;
            snprintf(ini_b.operacion, sizeof(ini_b.operacion), "%s", op);

            resp_t listo;
            if (escribir_exacto(b_fd, &ini_b, sizeof(ini_b)) != 1 ||
                leer_exacto(b_fd, &listo, sizeof(listo)) != 1 ||
                listo.tipo != TL_MSG_LISTO) {
                b_caido = 1;
            }
        }

        /* Liberar barrera para que el programa real ejecute execvp */
        char sig = 1;
        ssize_t w = write(barrera[1], &sig, 1);
        (void)w;
        close(barrera[1]);

        int estado_salida = 0;
        int64_t fin_prog_ns = 0;
        int vencio = esperar_con_plazo(pid_prog, plazo_s, &estado_salida, &fin_prog_ns);

        resp_t resp_b;
        memset(&resp_b, 0, sizeof(resp_b));
        if (!b_caido) {
            ctx_t term_b;
            memset(&term_b, 0, sizeof(term_b));
            term_b.tipo = TL_MSG_TERMINAR;
            term_b.solicitud = sol.solicitud;
            snprintf(term_b.trace_id, sizeof(term_b.trace_id), "%s", sa.trace_id);
            snprintf(term_b.parent_span, sizeof(term_b.parent_span), "%s", sa.span_id);
            term_b.pid_programa = es_app ? 0 : (int)pid_prog;
            term_b.pid_app = es_app ? pid_servicio : 0;
            term_b.estado_salida = estado_salida;
            snprintf(term_b.operacion, sizeof(term_b.operacion), "%s", op);

            if (escribir_exacto(b_fd, &term_b, sizeof(term_b)) != 1 ||
                leer_exacto(b_fd, &resp_b, sizeof(resp_b)) != 1 ||
                resp_b.tipo != TL_MSG_RESULTADO) {
                b_caido = 1;
            }
        }

        int estado_a = (vencio || estado_salida != 0 || b_caido) ? 1 : 0;
        char msg_a[64];
        char anom_a[24] = "";
        if (b_caido) {
            snprintf(msg_a, sizeof(msg_a), "servicio_b_caido");
            snprintf(anom_a, sizeof(anom_a), "error_proceso");
        } else if (vencio) {
            snprintf(msg_a, sizeof(msg_a), "plazo_vencido");
            snprintf(anom_a, sizeof(anom_a), "error_proceso");
        } else if (estado_salida != 0) {
            snprintf(msg_a, sizeof(msg_a), "exit_%d", estado_salida);
            snprintf(anom_a, sizeof(anom_a), "error_proceso");
        } else {
            snprintf(msg_a, sizeof(msg_a), "%.63s", sol.linea);
        }

        span_fin(&sa, estado_a, msg_a, anom_a);
        sa.fin = (!b_caido && resp_b.fin_ns > fin_prog_ns) ? resp_b.fin_ns : fin_prog_ns;
        if (sa.fin < sa.ini) {
            sa.fin = sa.ini;
        }
        sa.dur = (double)(sa.fin - sa.ini) / 1000000.0;

        if (spans_fd >= 0) {
            escribir_exacto(spans_fd, &sa, sizeof(span_t));
        }

        sol_resp_t sr;
        memset(&sr, 0, sizeof(sr));
        sr.tipo = TL_MSG_RESULTADO;
        sr.solicitud = sol.solicitud;
        sr.estado = estado_a;
        snprintf(sr.trace_id, sizeof(sr.trace_id), "%s", sa.trace_id);
        escribir_exacto(tl_fd, &sr, sizeof(sr));
    }

    if (b_fd >= 0) close(b_fd);
    if (spans_fd >= 0) close(spans_fd);
    close(tl_fd);
    close(srv_fd);
    unlink(a_sock);
    return 0;
}
