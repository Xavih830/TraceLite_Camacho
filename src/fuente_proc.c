#include "common.h"
#include "fuente.h"

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Implementación de fuente_t por muestreo periódico en /proc cada 20 ms */

#define MAX_PROCS_MUESTRA 2048
#define MAX_SEGUIDOS      512

typedef struct {
    int                pid;
    int                ppid;
    unsigned long      ticks;
    unsigned long long starttime;
} proc_stat_t;

typedef struct {
    int                pid;
    unsigned long long starttime;
    unsigned long      ticks_ini;
    unsigned long      ticks_ult;
    int                vivo;
} proc_seguido_t;

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_hilo;
static int g_hilo_activo = 0;
static int g_detener = 0;
static int g_pid_raiz = 0;
static unsigned long long g_start_raiz = 0;
static medida_t g_medida;
static proc_seguido_t g_seguidos[MAX_SEGUIDOS];
static int g_n_seguidos = 0;

static int es_digitos(const char *s) {
    if (s[0] == '\0') return 0;
    for (int i = 0; s[i] != '\0'; i++) {
        if (!isdigit((unsigned char)s[i])) return 0;
    }
    return 1;
}

static int leer_stat_completo(int pid, proc_stat_t *out) {
    char ruta[64];
    snprintf(ruta, sizeof(ruta), "/proc/%d/stat", pid);
    FILE *f = fopen(ruta, "r");
    if (f == NULL) {
        return -1;
    }
    char linea[1024];
    if (fgets(linea, sizeof(linea), f) == NULL) {
        fclose(f);
        return -1;
    }
    fclose(f);

    char *rp = strrchr(linea, ')');
    if (rp == NULL || rp[1] != ' ') {
        return -1;
    }

    char estado = '?';
    int ppid = 0;
    unsigned long ut = 0, st = 0;
    unsigned long long start = 0;
    int n = sscanf(rp + 2,
                   "%c %d %*s %*s %*s %*s %*s %*s %*s %*s %*s %lu %lu %*s %*s %*s %*s %*s %*s %llu",
                   &estado, &ppid, &ut, &st, &start);
    if (n < 5) {
        return -1;
    }
    (void)estado;
    out->pid = pid;
    out->ppid = ppid;
    out->ticks = ut + st;
    out->starttime = start;
    return 0;
}

static void actualizar_muestra(double ms_por_tick) {
    DIR *d = opendir("/proc");
    if (d == NULL) {
        return;
    }

    proc_stat_t *lista = (proc_stat_t *)malloc(sizeof(proc_stat_t) * MAX_PROCS_MUESTRA);
    if (lista == NULL) {
        closedir(d);
        return;
    }

    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && n < MAX_PROCS_MUESTRA) {
        if (!es_digitos(ent->d_name)) continue;
        int pid = atoi(ent->d_name);
        if (pid <= 0) continue;
        if (leer_stat_completo(pid, &lista[n]) == 0) {
            n++;
        }
    }
    closedir(d);

    pthread_mutex_lock(&g_mutex);
    for (int i = 0; i < g_n_seguidos; i++) {
        g_seguidos[i].vivo = 0;
    }

    /* Marcar raíz y descendientes por PPID */
    int en_sub[MAX_PROCS_MUESTRA];
    memset(en_sub, 0, sizeof(int) * (size_t)n);
    for (int i = 0; i < n; i++) {
        if (lista[i].pid == g_pid_raiz && lista[i].starttime == g_start_raiz) {
            en_sub[i] = 1;
            break;
        }
    }

    int cambio = 1;
    while (cambio) {
        cambio = 0;
        for (int i = 0; i < n; i++) {
            if (en_sub[i]) continue;
            for (int j = 0; j < n; j++) {
                if (en_sub[j] && lista[i].ppid == lista[j].pid &&
                    lista[i].starttime >= lista[j].starttime) {
                    en_sub[i] = 1;
                    cambio = 1;
                    break;
                }
            }
        }
    }

    int vivos = 0;
    unsigned long ticks_totales = 0;
    int64_t ahora = ahora_ns();

    for (int i = 0; i < n; i++) {
        if (!en_sub[i]) continue;
        vivos++;
        int idx = -1;
        for (int k = 0; k < g_n_seguidos; k++) {
            if (g_seguidos[k].pid == lista[i].pid &&
                g_seguidos[k].starttime == lista[i].starttime) {
                idx = k;
                break;
            }
        }
        if (idx >= 0) {
            g_seguidos[idx].vivo = 1;
            if (lista[i].ticks > g_seguidos[idx].ticks_ult) {
                g_seguidos[idx].ticks_ult = lista[i].ticks;
            }
        } else if (g_n_seguidos < MAX_SEGUIDOS) {
            proc_seguido_t *s = &g_seguidos[g_n_seguidos++];
            s->pid = lista[i].pid;
            s->starttime = lista[i].starttime;
            s->ticks_ini = lista[i].ticks;
            s->ticks_ult = lista[i].ticks;
            s->vivo = 1;
        }
    }

    for (int k = 0; k < g_n_seguidos; k++) {
        if (g_seguidos[k].ticks_ult >= g_seguidos[k].ticks_ini) {
            ticks_totales += (g_seguidos[k].ticks_ult - g_seguidos[k].ticks_ini);
        }
    }

    g_medida.n_procs = g_n_seguidos;
    g_medida.n_vivos = vivos;
    g_medida.cpu_ms = (double)ticks_totales * ms_por_tick;
    if (vivos > 0) {
        g_medida.ultima_actividad_ns = ahora;
    }
    pthread_mutex_unlock(&g_mutex);
    free(lista);
}

static void *hilo_muestreo(void *arg) {
    (void)arg;
    long clk = sysconf(_SC_CLK_TCK);
    double ms_por_tick = (clk > 0) ? (1000.0 / (double)clk) : 10.0;

    while (1) {
        pthread_mutex_lock(&g_mutex);
        int fin = g_detener;
        pthread_mutex_unlock(&g_mutex);
        if (fin) {
            break;
        }
        actualizar_muestra(ms_por_tick);
        dormir_ms(20);
    }
    return NULL;
}

static int proc_abrir(void) {
    return 0;
}

static int proc_iniciar(int pid_programa, int pid_app, int solicitud) {
    (void)solicitud;
    int objetivo = (pid_programa > 0) ? pid_programa : pid_app;
    proc_stat_t st;
    memset(&st, 0, sizeof(st));
    leer_stat_completo(objetivo, &st);

    pthread_mutex_lock(&g_mutex);
    g_detener = 0;
    g_pid_raiz = objetivo;
    g_start_raiz = st.starttime;
    g_n_seguidos = 0;
    memset(&g_medida, 0, sizeof(g_medida));
    snprintf(g_medida.atribucion, sizeof(g_medida.atribucion), "ventana");
    g_medida.ultima_actividad_ns = ahora_ns();

    if (st.pid > 0) {
        g_seguidos[g_n_seguidos].pid = st.pid;
        g_seguidos[g_n_seguidos].starttime = st.starttime;
        g_seguidos[g_n_seguidos].ticks_ini = st.ticks;
        g_seguidos[g_n_seguidos].ticks_ult = st.ticks;
        g_seguidos[g_n_seguidos].vivo = 1;
        g_n_seguidos++;
        g_medida.n_procs = 1;
        g_medida.n_vivos = 1;
    }
    pthread_mutex_unlock(&g_mutex);

    if (pthread_create(&g_hilo, NULL, hilo_muestreo, NULL) != 0) {
        return -1;
    }
    g_hilo_activo = 1;
    return 0;
}

static int proc_leer(medida_t *m) {
    pthread_mutex_lock(&g_mutex);
    *m = g_medida;
    pthread_mutex_unlock(&g_mutex);
    return 0;
}

static void proc_detener(void) {
    if (g_hilo_activo) {
        pthread_mutex_lock(&g_mutex);
        g_detener = 1;
        pthread_mutex_unlock(&g_mutex);
        pthread_join(g_hilo, NULL);
        g_hilo_activo = 0;
    }
}

static void proc_cerrar(void) {
    proc_detener();
}

const fuente_t fuente_proc = {
    .abrir   = proc_abrir,
    .iniciar = proc_iniciar,
    .leer    = proc_leer,
    .detener = proc_detener,
    .cerrar  = proc_cerrar
};
