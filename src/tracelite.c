#include "almacen.h"
#include "common.h"
#include "solicitudes.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

/* Orquestador TraceLite: descubre jerarquías de procesos en /proc y lanza collector + monitores */

#define MAX_PROCS_SO 2048
#define MAX_NODOS    32
#define MAX_RESUMEN  512

typedef struct {
    int           pid;
    int           ppid;
    char          comm[32];
    char          state;
    unsigned long ticks;
} proc_info_t;

typedef struct {
    int           pid;
    int           ppid;
    char          comm[32];
    char          cmdline[64];
    char          span_id[17];
    char          parent_span_id[17];
    unsigned long ticks_ini;
    unsigned long ticks_ult;
    int64_t       ini_ns;
    int64_t       fin_ns;
    int           finalizado;
    int           visto_ahora;
} resumen_proc_t;

static volatile sig_atomic_t detener_captura = 0;

static void manejar_senal(int sig) {
    (void)sig;
    detener_captura = 1;
}

static void instalar_manejador_senales(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = manejar_senal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

static void esperar_hijo(pid_t pid) {
    int st = 0;
    if (pid <= 0) {
        return;
    }
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
}

static int conectar_colector(const char *sock_path);

static void cerrar_colector(pid_t pid_col, const char *sock_path, const span_t *raiz) {
    int sock_fd = conectar_colector(sock_path);
    if (sock_fd >= 0) {
        if (raiz != NULL) {
            escribir_exacto(sock_fd, raiz, sizeof(span_t));
        }
        span_t fin;
        memset(&fin, 0, sizeof(fin));
        snprintf(fin.servicio, sizeof(fin.servicio), "__FIN__");
        escribir_exacto(sock_fd, &fin, sizeof(span_t));
        close(sock_fd);
    } else if (pid_col > 0) {
        kill(pid_col, SIGTERM);
    }
    esperar_hijo(pid_col);
}

typedef struct {
    int  pid;
    char comm[32];
    char span_id[17];
    char parent_span_id[17];
} nodo_obs_t;

static int es_numerico(const char *s) {
    if (s[0] == '\0') return 0;
    for (int i = 0; s[i] != '\0'; i++) {
        if (!isdigit((unsigned char)s[i])) return 0;
    }
    return 1;
}

static int leer_info_proc(int pid, proc_info_t *info) {
    char ruta_stat[64];
    snprintf(ruta_stat, sizeof(ruta_stat), "/proc/%d/stat", pid);
    FILE *fs = fopen(ruta_stat, "r");
    if (fs == NULL) return -1;

    char linea[1024];
    if (fgets(linea, sizeof(linea), fs) == NULL) {
        fclose(fs);
        return -1;
    }
    fclose(fs);

    char *lparen = strchr(linea, '(');
    char *rparen = strrchr(linea, ')');
    if (lparen == NULL || rparen == NULL || rparen <= lparen) return -1;

    size_t len = (size_t)(rparen - lparen - 1);
    if (len >= sizeof(info->comm)) len = sizeof(info->comm) - 1;
    memcpy(info->comm, lparen + 1, len);
    info->comm[len] = '\0';

    int ppid = 0;
    char state = '?';
    unsigned long utime = 0, stime = 0;
    int leidos = sscanf(rparen + 2,
                        "%c %d %*s %*s %*s %*s %*s %*s %*s %*s %*s %lu %lu",
                        &state, &ppid, &utime, &stime);
    if (leidos < 2) return -1;

    info->pid = pid;
    info->ppid = ppid;
    info->state = state;
    info->ticks = (leidos >= 4) ? (utime + stime) : 0;
    return 0;
}

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

static int cargar_procesos_so(proc_info_t *lista, int max_procs, int mi_pid) {
    DIR *dir = opendir("/proc");
    if (dir == NULL) return 0;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && count < max_procs) {
        if (!es_numerico(ent->d_name)) continue;
        int pid = atoi(ent->d_name);
        if (pid <= 0 || pid == mi_pid) continue;

        proc_info_t info;
        if (leer_info_proc(pid, &info) == 0) {
            if (strcmp(info.comm, "tracelite") != 0 &&
                strcmp(info.comm, "collector") != 0 &&
                strcmp(info.comm, "monitor_pid") != 0) {
                lista[count++] = info;
            }
        }
    }
    closedir(dir);
    return count;
}

static int contar_hijos(const proc_info_t *lista, int n, int pid) {
    int c = 0;
    for (int i = 0; i < n; i++) {
        if (lista[i].ppid == pid) c++;
    }
    return c;
}

/* Búsqueda de subcadena sin distinguir mayúsculas/minúsculas */
static int contiene_substr_ci(const char *haystack, const char *needle) {
    if (needle == NULL || needle[0] == '\0') return 0;
    size_t nlen = strlen(needle);
    for (size_t i = 0; haystack[i] != '\0'; i++) {
        size_t j = 0;
        while (j < nlen && haystack[i + j] != '\0' &&
               tolower((unsigned char)haystack[i + j]) == tolower((unsigned char)needle[j])) {
            j++;
        }
        if (j == nlen) return 1;
    }
    return 0;
}

/* Busca el PID raíz cuyo comm o cmdline coincida con el nombre dado */
static int buscar_pid_por_nombre(const proc_info_t *lista, int n, const char *nombre) {
    int ppid_propio = (int)getppid();

    for (int pasada = 0; pasada < 2; pasada++) {
        int mejor_pid = -1;
        int mejor_es_raiz = -1;
        int mejor_hijos = -1;

        for (int i = 0; i < n; i++) {
            int coincide = 0;
            if (pasada == 0) {
                coincide = contiene_substr_ci(lista[i].comm, nombre);
            } else {
                if (lista[i].pid == ppid_propio) continue;
                char cmd[128];
                leer_cmdline_proc(lista[i].pid, lista[i].comm, cmd, sizeof(cmd));
                coincide = contiene_substr_ci(cmd, nombre);
            }
            if (!coincide) continue;

            int es_raiz = 1;
            for (int k = 0; k < n; k++) {
                if (lista[k].pid == lista[i].ppid &&
                    contiene_substr_ci(lista[k].comm, nombre)) {
                    es_raiz = 0;
                    break;
                }
            }

            int h = contar_hijos(lista, n, lista[i].pid);
            if (es_raiz > mejor_es_raiz ||
                (es_raiz == mejor_es_raiz && h > mejor_hijos)) {
                mejor_es_raiz = es_raiz;
                mejor_hijos = h;
                mejor_pid = lista[i].pid;
            }
        }

        if (mejor_pid > 0) {
            return mejor_pid;
        }
    }
    return -1;
}

static void imprimir_ayuda(const char *prog) {
    printf("Uso:\n");
    printf("  Observación: %s [PID | NOMBRE] [-r] [-d SEG] [--interval MS]\n", prog);
    printf("  App viva:    %s --app <NOMBRE|PID> [--resumen] [-d SEG] [--plazo SEG] [--modo MODO] -- <orden...>\n", prog);
    printf("  Historial:   %s --historial [--trace ID]\n\n", prog);
    printf("Opciones:\n");
    printf("  -r, --resumen          En observación: captura continua; en --app: serie de solicitudes\n");
    printf("  -d, --duracion <seg>   Duración en segundos de la ventana de observación\n");
    printf("  --interval <ms>        Intervalo de muestreo en ms (solo observación, por defecto 150)\n");
    printf("  --app <nombre|PID>     Aplicación viva del SO a observar\n");
    printf("  --unidad <servicio>    Unidad systemd de la aplicación viva\n");
    printf("  --fin <orden|inact.>   Cierre por orden o inactividad (por defecto inactividad)\n");
    printf("  --linea-base <seg>     Segundos de calibración previa (0 omite calibración)\n");
    printf("  --modo <modo>          respuesta | estabilidad | escalado | fallo\n");
    printf("  --slo-ms <ms>          Umbral máximo esperado en modo respuesta\n");
    printf("  --entradas <lista>     Lista separada por comas de entradas por solicitud\n");
    printf("  --tamanos <lista>      Tamaños asociados para modo escalado\n");
    printf("  --fallo-en <N>         Inyecta fallo en la solicitud N (modo fallo)\n");
    printf("  --plazo <seg>          Plazo máximo manual por solicitud en segundos\n");
    printf("  -n <N>                 Número de solicitudes (5 por defecto, con --resumen)\n");
    printf("  --fuente <auto|proc>   Fuente de observación (auto | ebpf | proc)\n");
    printf("  --mostrar-salida       Muestra stdout/stderr de la orden ejecutada\n");
    printf("  -h, --help             Muestra esta ayuda y ejemplos\n\n");
    printf("Ejemplos:\n");
    printf("  %s --resumen -d 10 --interval 150 antigravity-ide\n", prog);
    printf("  %s --resumen -d 10 --app antigravity-ide --fin orden --linea-base 0 --modo estabilidad --entradas \"10,10,10,10,10\" -- ./docs/pruebas/tsp_np.py\n", prog);
}

/* Selección automática de PID raíz con mayor jerarquía de hijos */
static int elegir_pid_raiz_automatico(const proc_info_t *lista, int n) {
    /* Prioriza un proceso con hijos reales distinto de PID 1/2 */
    int candidato = -1;
    int max_hijos = 0;
    for (int i = 0; i < n; i++) {
        if (lista[i].pid <= 2) continue;
        int h = contar_hijos(lista, n, lista[i].pid);
        if (h > max_hijos) {
            max_hijos = h;
            candidato = lista[i].pid;
        }
    }
    if (candidato > 0) return candidato;

    /* Respaldo: PID 1 o PID 2 si tienen hijos */
    for (int i = 0; i < n; i++) {
        if (contar_hijos(lista, n, lista[i].pid) > 0) {
            return lista[i].pid;
        }
    }
    return (n > 0) ? lista[0].pid : -1;
}

static void construir_subarbol(const proc_info_t *lista, int n_procs,
                               int pid_actual, const char *parent_span_id,
                               nodo_obs_t *nodos, int *n_nodos, int profundidad) {
    if (*n_nodos >= MAX_NODOS || profundidad > 6) {
        return;
    }

    const proc_info_t *actual = NULL;
    for (int i = 0; i < n_procs; i++) {
        if (lista[i].pid == pid_actual) {
            actual = &lista[i];
            break;
        }
    }
    if (actual == NULL) return;

    int idx = (*n_nodos)++;
    nodos[idx].pid = actual->pid;
    snprintf(nodos[idx].comm, sizeof(nodos[idx].comm), "%s", actual->comm);
    id_hex(nodos[idx].span_id, 8);
    snprintf(nodos[idx].parent_span_id, sizeof(nodos[idx].parent_span_id), "%s", parent_span_id);

    for (int i = 0; i < n_procs && *n_nodos < MAX_NODOS; i++) {
        if (lista[i].ppid == pid_actual) {
            construir_subarbol(lista, n_procs, lista[i].pid,
                               nodos[idx].span_id, nodos, n_nodos, profundidad + 1);
        }
    }
}

static int conectar_colector(const char *sock_path) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);

    for (int intento = 0; intento < 20; intento++) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            return fd;
        }
        close(fd);
        dormir_ms(25);
    }
    return -1;
}

static void recolectar_subarbol_proc(const proc_info_t *lista, int n_procs,
                                     int pid_actual, const proc_info_t **out_procs,
                                     int *n_out, int max_out, int profundidad) {
    if (*n_out >= max_out || profundidad > 10) return;

    const proc_info_t *actual = NULL;
    for (int i = 0; i < n_procs; i++) {
        if (lista[i].pid == pid_actual) {
            actual = &lista[i];
            break;
        }
    }
    if (actual == NULL) return;

    out_procs[(*n_out)++] = actual;

    for (int i = 0; i < n_procs && *n_out < max_out; i++) {
        if (lista[i].ppid == pid_actual) {
            recolectar_subarbol_proc(lista, n_procs, lista[i].pid,
                                     out_procs, n_out, max_out, profundidad + 1);
        }
    }
}

static int ejecutar_modo_resumen(int pid_raiz, const char *sock_path,
                                 const char *ruta_col,
                                 int interval_ms, int duracion_seg) {
    fflush(stdout);
    pid_t pid_col = fork();
    if (pid_col < 0) {
        perror("fork collector");
        return 1;
    }
    if (pid_col == 0) {
        signal(SIGINT, SIG_IGN);
        char *args_col[3] = { (char *)ruta_col, (char *)sock_path, NULL };
        execv(ruta_col, args_col);
        perror("execv collector");
        _exit(127);
    }

    span_t raiz;
    span_ini(&raiz, NULL, NULL, "tracelite", "resumen_arbol_so");

    if (duracion_seg > 0) {
        printf("[tracelite] Modo --resumen activo sobre PID %d (%d s, intervalo %d ms). Capturando...\n",
               pid_raiz, duracion_seg, interval_ms);
    } else {
        printf("[tracelite] Modo --resumen activo sobre PID %d (intervalo %d ms). Ctrl+C para imprimir tabla.\n",
               pid_raiz, interval_ms);
    }
    fflush(stdout);

    proc_info_t *lista = (proc_info_t *)malloc(sizeof(proc_info_t) * MAX_PROCS_SO);
    resumen_proc_t *hist = (resumen_proc_t *)calloc(MAX_RESUMEN, sizeof(resumen_proc_t));
    const proc_info_t **subarbol = (const proc_info_t **)malloc(sizeof(proc_info_t *) * MAX_RESUMEN);
    if (lista == NULL || hist == NULL || subarbol == NULL) {
        free(lista);
        free(hist);
        free(subarbol);
        cerrar_colector(pid_col, sock_path, NULL);
        return 1;
    }

    int n_hist = 0;
    int64_t t_inicio_ns = ahora_ns();
    int64_t limite_ns = (duracion_seg > 0) ? (int64_t)duracion_seg * 1000000000LL : 0;
    int ultimo_seg_mostrado = -1;

    while (!detener_captura) {
        int n_procs = cargar_procesos_so(lista, MAX_PROCS_SO, (int)getpid());
        int n_sub = 0;
        recolectar_subarbol_proc(lista, n_procs, pid_raiz, subarbol, &n_sub, MAX_RESUMEN, 0);

        for (int i = 0; i < n_hist; i++) {
            hist[i].visto_ahora = 0;
        }

        int64_t t_now = ahora_ns();
        for (int i = 0; i < n_sub; i++) {
            const proc_info_t *p = subarbol[i];
            int idx = -1;
            for (int j = 0; j < n_hist; j++) {
                if (!hist[j].finalizado && hist[j].pid == p->pid) {
                    idx = j;
                    break;
                }
            }

            if (idx >= 0) {
                hist[idx].visto_ahora = 1;
                hist[idx].ticks_ult = p->ticks;
                hist[idx].fin_ns = t_now;
            } else if (n_hist < MAX_RESUMEN) {
                resumen_proc_t *r = &hist[n_hist++];
                r->pid = p->pid;
                r->ppid = p->ppid;
                snprintf(r->comm, sizeof(r->comm), "%s", p->comm);
                leer_cmdline_proc(p->pid, p->comm, r->cmdline, sizeof(r->cmdline));
                id_hex(r->span_id, 8);
                snprintf(r->parent_span_id, sizeof(r->parent_span_id), "%s", raiz.span_id);
                for (int k = n_hist - 2; k >= 0; k--) {
                    if (!hist[k].finalizado && hist[k].pid == p->ppid) {
                        snprintf(r->parent_span_id, sizeof(r->parent_span_id), "%s", hist[k].span_id);
                        break;
                    }
                }
                r->ticks_ini = p->ticks;
                r->ticks_ult = p->ticks;
                r->ini_ns = t_now;
                r->fin_ns = t_now;
                r->finalizado = 0;
                r->visto_ahora = 1;
            }
        }

        for (int i = 0; i < n_hist; i++) {
            if (!hist[i].finalizado && !hist[i].visto_ahora) {
                hist[i].finalizado = 1;
                hist[i].fin_ns = t_now;
            }
        }

        if (n_sub == 0) {
            break;
        }
        int seg_transcurridos = (int)((t_now - t_inicio_ns) / 1000000000LL);
        if (seg_transcurridos != ultimo_seg_mostrado) {
            ultimo_seg_mostrado = seg_transcurridos;
            if (duracion_seg > 0) {
                printf("\r[tracelite] Capturando PID %d: %2d / %d s (%d procesos registrados)...",
                       pid_raiz, seg_transcurridos, duracion_seg, n_hist);
            } else {
                printf("\r[tracelite] Capturando PID %d: %2d s (%d procesos registrados)...",
                       pid_raiz, seg_transcurridos, n_hist);
            }
            fflush(stdout);
        }
        if (limite_ns > 0 && (t_now - t_inicio_ns) >= limite_ns) {
            break;
        }

        dormir_ms(interval_ms);
    }
    printf("\n");

    span_fin(&raiz, (n_hist == 0) ? 1 : 0, (n_hist == 0) ? "pid_no_encontrado" : "", "");
    raiz.n_procs = n_hist;

    if (n_hist > 0) {
        int fd_col = conectar_colector(sock_path);
        if (fd_col >= 0) {
            long clk_tck = sysconf(_SC_CLK_TCK);
            if (clk_tck <= 0) clk_tck = 100;
            for (int i = 0; i < n_hist; i++) {
                const resumen_proc_t *r = &hist[i];
                span_t s;
                memset(&s, 0, sizeof(s));
                snprintf(s.trace_id, sizeof(s.trace_id), "%s", raiz.trace_id);
                snprintf(s.span_id, sizeof(s.span_id), "%s", r->span_id);
                snprintf(s.parent, sizeof(s.parent), "%s", r->parent_span_id);
                snprintf(s.servicio, sizeof(s.servicio), "%s", r->comm);
                snprintf(s.operacion, sizeof(s.operacion), "%s", r->cmdline[0] ? r->cmdline : r->comm);
                s.ini = r->ini_ns;
                s.fin = (r->fin_ns > r->ini_ns) ? r->fin_ns : (r->ini_ns + 1000000LL);
                s.dur = (double)(s.fin - s.ini) / 1000000.0;
                s.pid = r->pid;
                s.n_procs = 1;
                if (r->ticks_ult >= r->ticks_ini) {
                    s.cpu_ms = (double)(r->ticks_ult - r->ticks_ini) * 1000.0 / (double)clk_tck;
                }
                snprintf(s.atribucion, sizeof(s.atribucion), "exacta");
                escribir_exacto(fd_col, &s, sizeof(span_t));
            }
            close(fd_col);
        }
    }

    cerrar_colector(pid_col, sock_path, &raiz);

    free(lista);
    free(hist);
    free(subarbol);
    return 0;
}

int main(int argc, char *argv[]) {
    int modo_resumen = 0;
    int interval_ms = 150;
    int interval_set = 0;
    int duracion_seg = -1;
    int pid_arg = -1;
    const char *nombre_arg = NULL;

    int n_sol = -1;
    int plazo_s = 60;
    int plazo_set = 0;
    int mostrar_salida = 0;
    const char *fuente = "auto";
    int idx_cmd = -1;
    int modo_historial = 0;
    const char *trace_filtro = NULL;
    int ret_dias = 7;
    int ret_seg = 0;
    const char *unidad = NULL;
    int linea_base_s = 5;
    const char *app_target = NULL;
    int fin_por_orden = 0;
    const char *modo_str = NULL;
    double slo_ms = 0.0;
    const char *entradas = NULL;
    const char *tamanos = NULL;
    int fallo_en = 5;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            idx_cmd = i + 1;
            break;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            imprimir_ayuda(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--historial") == 0) {
            modo_historial = 1;
        } else if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
            trace_filtro = argv[++i];
        } else if (strcmp(argv[i], "--retencion-dias") == 0 && i + 1 < argc) {
            ret_dias = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--retencion-seg") == 0 && i + 1 < argc) {
            ret_seg = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--unidad") == 0 && i + 1 < argc) {
            unidad = argv[++i];
        } else if (strcmp(argv[i], "--linea-base") == 0 && i + 1 < argc) {
            linea_base_s = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--app") == 0 && i + 1 < argc) {
            app_target = argv[++i];
        } else if (strcmp(argv[i], "--fin") == 0 && i + 1 < argc) {
            const char *fv = argv[++i];
            if (strcmp(fv, "orden") == 0) fin_por_orden = 1;
            else if (strcmp(fv, "inactividad") == 0) fin_por_orden = 0;
            else {
                fprintf(stderr, "[tracelite] --fin inválido: %s (usa orden|inactividad)\n", fv);
                return 2;
            }
        } else if (strcmp(argv[i], "--modo") == 0 && i + 1 < argc) {
            modo_str = argv[++i];
        } else if (strcmp(argv[i], "--slo-ms") == 0 && i + 1 < argc) {
            slo_ms = atof(argv[++i]);
        } else if (strcmp(argv[i], "--entradas") == 0 && i + 1 < argc) {
            entradas = argv[++i];
        } else if (strcmp(argv[i], "--tamanos") == 0 && i + 1 < argc) {
            tamanos = argv[++i];
        } else if (strcmp(argv[i], "--fallo-en") == 0 && i + 1 < argc) {
            fallo_en = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--resumen") == 0 || strcmp(argv[i], "-r") == 0) {
            modo_resumen = 1;
        } else if (strcmp(argv[i], "--interval") == 0 && i + 1 < argc) {
            interval_ms = atoi(argv[++i]);
            if (interval_ms < 50) interval_ms = 50;
            interval_set = 1;
        } else if ((strcmp(argv[i], "--duracion") == 0 || strcmp(argv[i], "-d") == 0) && i + 1 < argc) {
            duracion_seg = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            n_sol = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--plazo") == 0 && i + 1 < argc) {
            plazo_s = atoi(argv[++i]);
            plazo_set = 1;
        } else if (strcmp(argv[i], "--fuente") == 0 && i + 1 < argc) {
            fuente = argv[++i];
        } else if (strcmp(argv[i], "--mostrar-salida") == 0) {
            mostrar_salida = 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "[tracelite] Opción desconocida o incompleta: %s\n", argv[i]);
            return 2;
        } else if (es_numerico(argv[i])) {
            pid_arg = atoi(argv[i]);
        } else {
            nombre_arg = argv[i];
        }
    }

    if (modo_historial) {
        almacen_indice_t alm;
        if (almacen_cargar(&alm, NULL, ret_dias, ret_seg) != 0) {
            fprintf(stderr, "[tracelite] No se pudo abrir el almacén de historial.\n");
            return 1;
        }
        int rc = almacen_mostrar_historial(&alm, trace_filtro);
        almacen_liberar(&alm);
        return rc;
    }

    int es_app_dest = (app_target != NULL || unidad != NULL);
    int es_solicitud = (idx_cmd >= 0 || es_app_dest);
    int tiene_opc_sol = (n_sol >= 0 || plazo_set || mostrar_salida ||
                         strcmp(fuente, "auto") != 0 || modo_str != NULL);

    modo_t modo = modo_resumen ? MODO_ESTABILIDAD : MODO_RESPUESTA;
    if (modo_str != NULL) {
        if (strcmp(modo_str, "respuesta") == 0) modo = MODO_RESPUESTA;
        else if (strcmp(modo_str, "estabilidad") == 0) modo = MODO_ESTABILIDAD;
        else if (strcmp(modo_str, "escalado") == 0) modo = MODO_ESCALADO;
        else if (strcmp(modo_str, "fallo") == 0) modo = MODO_FALLO;
        else {
            fprintf(stderr, "[tracelite] --modo inválido: %s\n", modo_str);
            return 2;
        }
    }

    tiempo_nivel_t tnivel = plazo_set ? TIEMPO_MANUAL : TIEMPO_NORMAL;

    if (es_solicitud) {
        if (!es_app_dest) {
            fprintf(stderr, "[tracelite] Solo se admiten aplicaciones vivas (--app o --unidad).\n");
            return 2;
        }
        if (!modo_resumen && (modo == MODO_ESCALADO || modo == MODO_FALLO)) {
            fprintf(stderr, "[tracelite] --modo escalado y --modo fallo exigen --resumen.\n");
            return 2;
        }
        if (pid_arg > 0 || nombre_arg != NULL) {
            fprintf(stderr, "[tracelite] Las opciones de solicitud no se combinan con PID o nombre.\n");
            return 2;
        }
        if (idx_cmd < 0) {
            fprintf(stderr, "[tracelite] Destino --app/--unidad requiere -- <orden>.\n");
            return 2;
        }
        if (interval_set) {
            fprintf(stderr, "[tracelite] --interval solo aplica en modo observación.\n");
            return 2;
        }
        if (n_sol >= 0 && !modo_resumen) {
            fprintf(stderr, "[tracelite] -n solo aplica con --resumen.\n");
            return 2;
        }
        if (idx_cmd >= argc) {
            fprintf(stderr, "[tracelite] Falta el comando después de --.\n");
            return 2;
        }
        if (strcmp(fuente, "auto") != 0 && strcmp(fuente, "ebpf") != 0 &&
            strcmp(fuente, "proc") != 0) {
            fprintf(stderr, "[tracelite] --fuente inválida: %s (usa auto|ebpf|proc)\n", fuente);
            return 2;
        }
        if (plazo_s <= 0) {
            fprintf(stderr, "[tracelite] --plazo debe ser mayor que 0.\n");
            return 2;
        }
    } else if (tiene_opc_sol) {
        fprintf(stderr, "[tracelite] Opciones de solicitud requieren --app <nombre|PID> -- <orden>.\n");
        return 2;
    }

    char dir_priv[PATH_MAX];
    char sock_path[PATH_MAX];
    if (crear_dir_privado(dir_priv, sizeof(dir_priv)) != 0) {
        return 1;
    }
    int n_sock = snprintf(sock_path, sizeof(sock_path), "%s/spans.sock", dir_priv);
    if (n_sock < 0 || (size_t)n_sock >= sizeof(sock_path) || validar_ruta_socket(sock_path) != 0) {
        rmdir(dir_priv);
        return 1;
    }

    instalar_manejador_senales();

    if (es_solicitud) {
        char cmd_linea[512] = "";
        if (idx_cmd >= 0) {
            size_t pos = 0;
            for (int i = idx_cmd; i < argc && pos + 2 < sizeof(cmd_linea); i++) {
                int w = snprintf(cmd_linea + pos, sizeof(cmd_linea) - pos,
                                 "%s%s", (i == idx_cmd) ? "" : " ", argv[i]);
                if (w > 0) pos += (size_t)w;
            }
        }
        solicitudes_cfg_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.resumen = modo_resumen;
        cfg.n_solicitudes = (n_sol > 0) ? n_sol : (modo_resumen ? 5 : 1);
        cfg.ventana_seg = (duracion_seg > 0) ? duracion_seg : 0;
        if (plazo_set) {
            cfg.plazo_s = plazo_s;
        } else if (modo == MODO_ESCALADO) {
            cfg.plazo_s = 30;
        } else if (modo == MODO_ESTABILIDAD) {
            cfg.plazo_s = 60;
        } else {
            cfg.plazo_s = 10;
        }
        if (!plazo_set && cfg.ventana_seg > 0 && cfg.plazo_s < cfg.ventana_seg + 2) {
            cfg.plazo_s = cfg.ventana_seg + 2;
        }
        cfg.mostrar_salida = mostrar_salida;
        cfg.cmd_linea = cmd_linea;
        cfg.fuente = fuente;
        cfg.ret_dias = ret_dias;
        cfg.ret_seg = ret_seg;
        cfg.destino = DEST_APP;
        cfg.unidad = unidad;
        cfg.linea_base_s = (linea_base_s >= 0) ? linea_base_s : 5;
        cfg.app_target = app_target;
        cfg.fin_por_orden = fin_por_orden;
        cfg.modo = modo;
        cfg.tiempo_nivel = tnivel;
        cfg.slo_ms = slo_ms;
        cfg.entradas = entradas;
        cfg.tamanos = tamanos;
        cfg.fallo_en = fallo_en;

        int rc = ejecutar_solicitudes(&cfg, dir_priv, sock_path, &detener_captura);
        rmdir(dir_priv);
        return rc;
    }

    proc_info_t *lista = (proc_info_t *)malloc(sizeof(proc_info_t) * MAX_PROCS_SO);
    if (lista == NULL) {
        rmdir(dir_priv);
        return 1;
    }

    int n_procs = cargar_procesos_so(lista, MAX_PROCS_SO, (int)getpid());
    if (n_procs == 0) {
        fprintf(stderr, "[tracelite] No se encontraron procesos accesibles en /proc.\n");
        free(lista);
        rmdir(dir_priv);
        return 1;
    }

    int pid_raiz = -1;
    if (pid_arg > 0) {
        pid_raiz = pid_arg;
    } else if (nombre_arg != NULL) {
        pid_raiz = buscar_pid_por_nombre(lista, n_procs, nombre_arg);
        if (pid_raiz > 0) {
            printf("[tracelite] Proceso '%s' resuelto al PID raíz %d.\n", nombre_arg, pid_raiz);
        } else if (modo_resumen) {
            fprintf(stderr, "[tracelite] No se encontró ningún proceso activo para '%s'.\n", nombre_arg);
            free(lista);
            rmdir(dir_priv);
            return 1;
        }
    } else {
        pid_raiz = elegir_pid_raiz_automatico(lista, n_procs);
    }

    if (pid_raiz <= 0 && nombre_arg == NULL) {
        fprintf(stderr, "[tracelite] PID raíz inválido.\n");
        free(lista);
        rmdir(dir_priv);
        return 1;
    }

    char ruta_col[PATH_MAX];
    char ruta_mon[PATH_MAX];
    if (ruta_hermana("collector", ruta_col, sizeof(ruta_col)) != 0 ||
        ruta_hermana("monitor_pid", ruta_mon, sizeof(ruta_mon)) != 0) {
        fprintf(stderr, "[tracelite] No se pudieron resolver rutas de binarios hermanos.\n");
        free(lista);
        rmdir(dir_priv);
        return 1;
    }

    if (modo_resumen) {
        free(lista);
        if (duracion_seg < 0) {
            duracion_seg = 60;
        }
        int rc = ejecutar_modo_resumen(pid_raiz, sock_path, ruta_col, interval_ms, duracion_seg);
        unlink(sock_path);
        rmdir(dir_priv);
        return rc;
    }

    /* 1. Lanzar collector con fork() + execv() */
    fflush(stdout);
    pid_t pid_col = fork();
    if (pid_col < 0) {
        perror("fork collector");
        rmdir(dir_priv);
        free(lista);
        return 1;
    }
    if (pid_col == 0) {
        signal(SIGINT, SIG_IGN);
        char *args_col[3] = { ruta_col, sock_path, NULL };
        execv(ruta_col, args_col);
        perror("execv collector");
        _exit(127);
    }

    /* 2. Crear span raíz del orquestador y construir el árbol real de PIDs */
    span_t raiz;
    span_ini(&raiz, NULL, NULL, "tracelite", "arbol_ppid_so");

    nodo_obs_t nodos[MAX_NODOS];
    int n_nodos = 0;
    if (pid_raiz > 0) {
        construir_subarbol(lista, n_procs, pid_raiz, raiz.span_id, nodos, &n_nodos, 0);
    }
    free(lista);

    if (n_nodos == 0) {
        if (nombre_arg != NULL && pid_raiz <= 0) {
            fprintf(stderr, "[tracelite] No se encontró ningún proceso activo para '%s'.\n", nombre_arg);
            span_fin(&raiz, 1, "proceso_no_encontrado", "error_proceso");
        } else {
            fprintf(stderr, "[tracelite] El PID %d no está en /proc o no es accesible.\n", pid_raiz);
            span_fin(&raiz, 1, "pid_no_encontrado", "error_proceso");
        }
    } else {
        printf("[tracelite] Árbol descubierto desde PID raíz %d (%d procesos):\n",
               pid_raiz, n_nodos);
        for (int i = 0; i < n_nodos; i++) {
            printf("  - PID %-6d (%s)\n", nodos[i].pid, nodos[i].comm);
        }
    }

    /* 3. Lanzar un monitor_pid en paralelo por cada proceso del árbol */
    pid_t hijos[MAX_NODOS];
    for (int i = 0; i < MAX_NODOS; i++) {
        hijos[i] = -1;
    }
    for (int i = 0; i < n_nodos && !detener_captura; i++) {
        fflush(stdout);
        hijos[i] = fork();
        if (hijos[i] == 0) {
            char pid_str[16];
            snprintf(pid_str, sizeof(pid_str), "%d", nodos[i].pid);
            char *args_mon[8] = {
                ruta_mon,
                pid_str,
                raiz.trace_id,
                nodos[i].span_id,
                nodos[i].parent_span_id,
                sock_path,
                "250",
                NULL
            };
            execv(ruta_mon, args_mon);
            perror("execv monitor_pid");
            _exit(127);
        }
    }

    for (int i = 0; i < n_nodos; i++) {
        if (hijos[i] > 0) {
            esperar_hijo(hijos[i]);
        }
    }

    if (n_nodos > 0) {
        span_fin(&raiz, 0, "", "");
    }

    cerrar_colector(pid_col, sock_path, &raiz);
    unlink(sock_path);
    rmdir(dir_priv);
    return 0;
}
