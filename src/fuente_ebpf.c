#include "common.h"
#include "ebpf_eventos.h"
#include "fuente.h"
#include "trace.skel.h"

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Implementación de fuente_t usando libbpf CO-RE y BPF_MAP_TYPE_RINGBUF */

static struct trace_bpf *g_skel = NULL;
static struct ring_buffer *g_rb = NULL;
static pthread_t g_hilo;
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static int g_hilo_activo = 0;
static int g_detener = 0;
static uint32_t g_sol_actual = 0;
static uint32_t g_puerto_cli = 0;
static medida_t g_medida;

struct conn_info_user {
    uint32_t solicitud;
    uint32_t puerto_backend;
};

static int suprimir_libbpf_print(enum libbpf_print_level level, const char *format, va_list args) {
    (void)level;
    (void)format;
    (void)args;
    return 0;
}

static int manejar_evento(void *ctx, void *data, size_t len) {
    (void)ctx;
    if (len < sizeof(struct ebpf_evento)) {
        return 0;
    }
    const struct ebpf_evento *ev = (const struct ebpf_evento *)data;

    pthread_mutex_lock(&g_mtx);
    if (ev->solicitud == g_sol_actual) {
        if (ev->tipo == EV_FORK) {
            g_medida.n_procs++;
            g_medida.n_vivos++;
        } else if (ev->tipo == EV_EXIT) {
            if (g_medida.n_vivos > 0) {
                g_medida.n_vivos--;
            }
        }
        if ((int64_t)ev->ts_ns > g_medida.ultima_actividad_ns) {
            g_medida.ultima_actividad_ns = (int64_t)ev->ts_ns;
        }
    }
    pthread_mutex_unlock(&g_mtx);
    return 0;
}

static void *hilo_ringbuf(void *arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&g_mtx);
        int fin = g_detener;
        pthread_mutex_unlock(&g_mtx);
        if (fin) {
            ring_buffer__poll(g_rb, 0);
            break;
        }
        ring_buffer__poll(g_rb, 20);
    }
    return NULL;
}

static int ebpf_abrir(void) {
    libbpf_set_print(suprimir_libbpf_print);
    g_skel = trace_bpf__open_and_load();
    if (g_skel == NULL) {
        return -1;
    }
    if (trace_bpf__attach(g_skel) != 0) {
        trace_bpf__destroy(g_skel);
        g_skel = NULL;
        return -1;
    }
    g_rb = ring_buffer__new(bpf_map__fd(g_skel->maps.eventos), manejar_evento, NULL, NULL);
    if (g_rb == NULL) {
        trace_bpf__destroy(g_skel);
        g_skel = NULL;
        return -1;
    }
    return 0;
}

static int ebpf_iniciar(int pid_programa, int pid_app, int solicitud) {
    if (g_skel == NULL) return -1;

    uint32_t tgid = (uint32_t)((pid_programa > 0) ? pid_programa : pid_app);
    uint32_t sol = (uint32_t)solicitud;
    uint64_t cero = 0;

    int fd_cpu = bpf_map__fd(g_skel->maps.cpu);
    int fd_marc = bpf_map__fd(g_skel->maps.marcados);
    bpf_map_update_elem(fd_cpu, &sol, &cero, BPF_ANY);

    pthread_mutex_lock(&g_mtx);
    g_detener = 0;
    g_sol_actual = sol;
    memset(&g_medida, 0, sizeof(g_medida));
    g_medida.n_procs = 1;
    g_medida.n_vivos = 1;
    g_medida.ultima_actividad_ns = ahora_ns();
    snprintf(g_medida.atribucion, sizeof(g_medida.atribucion), "exacta");
    pthread_mutex_unlock(&g_mtx);

    if (bpf_map_update_elem(fd_marc, &tgid, &sol, BPF_ANY) != 0) {
        return -1;
    }

    if (pthread_create(&g_hilo, NULL, hilo_ringbuf, NULL) != 0) {
        return -1;
    }
    g_hilo_activo = 1;
    return 0;
}

static int ebpf_iniciar_conn(int puerto_cliente, int puerto_backend, int solicitud) {
    if (g_skel == NULL) return -1;

    uint32_t sol = (uint32_t)solicitud;
    uint32_t pcli = (uint32_t)puerto_cliente;
    uint64_t cero = 0;
    struct conn_info_user info = { sol, (uint32_t)puerto_backend };

    bpf_map_update_elem(bpf_map__fd(g_skel->maps.cpu), &sol, &cero, BPF_ANY);
    bpf_map_update_elem(bpf_map__fd(g_skel->maps.conexiones), &pcli, &info, BPF_ANY);

    pthread_mutex_lock(&g_mtx);
    g_detener = 0;
    g_sol_actual = sol;
    g_puerto_cli = pcli;
    memset(&g_medida, 0, sizeof(g_medida));
    g_medida.n_procs = 1;
    g_medida.n_vivos = 1;
    g_medida.ultima_actividad_ns = ahora_ns();
    snprintf(g_medida.atribucion, sizeof(g_medida.atribucion), "conexion");
    pthread_mutex_unlock(&g_mtx);

    if (pthread_create(&g_hilo, NULL, hilo_ringbuf, NULL) != 0) {
        return -1;
    }
    g_hilo_activo = 1;
    return 0;
}

static unsigned long leer_perdidos(void) {
    if (g_skel == NULL) return 0;
    int ncpus = libbpf_num_possible_cpus();
    if (ncpus <= 0) return 0;
    uint64_t *vals = (uint64_t *)calloc((size_t)ncpus, sizeof(uint64_t));
    if (vals == NULL) return 0;

    uint32_t k = 0;
    unsigned long tot = 0;
    if (bpf_map_lookup_elem(bpf_map__fd(g_skel->maps.perdidos), &k, vals) == 0) {
        for (int i = 0; i < ncpus; i++) {
            tot += (unsigned long)vals[i];
        }
    }
    free(vals);
    return tot;
}

static int ebpf_leer(medida_t *m) {
    uint64_t cpu_ns = 0;
    uint64_t ult_ns = 0;
    if (g_skel != NULL) {
        uint32_t sol = g_sol_actual;
        bpf_map_lookup_elem(bpf_map__fd(g_skel->maps.cpu), &sol, &cpu_ns);
        bpf_map_lookup_elem(bpf_map__fd(g_skel->maps.ultima_act), &sol, &ult_ns);
    }
    unsigned long perd = leer_perdidos();

    pthread_mutex_lock(&g_mtx);
    g_medida.cpu_ms = (double)cpu_ns / 1000000.0;
    g_medida.perdidos = perd;
    if ((int64_t)ult_ns > g_medida.ultima_actividad_ns) {
        g_medida.ultima_actividad_ns = (int64_t)ult_ns;
    }
    *m = g_medida;
    pthread_mutex_unlock(&g_mtx);
    return 0;
}

static void ebpf_detener(void) {
    if (g_hilo_activo) {
        pthread_mutex_lock(&g_mtx);
        g_detener = 1;
        pthread_mutex_unlock(&g_mtx);
        pthread_join(g_hilo, NULL);
        g_hilo_activo = 0;
    }
    if (g_skel != NULL) {
        if (g_puerto_cli > 0) {
            uint32_t p = g_puerto_cli;
            bpf_map_delete_elem(bpf_map__fd(g_skel->maps.conexiones), &p);
            g_puerto_cli = 0;
        }
        int fd_h = bpf_map__fd(g_skel->maps.hilos_marcados);
        uint32_t k = 0, nk = 0;
        while (bpf_map_get_next_key(fd_h, k ? &k : NULL, &nk) == 0) {
            uint32_t val = 0;
            if (bpf_map_lookup_elem(fd_h, &nk, &val) == 0 && val == g_sol_actual) {
                bpf_map_delete_elem(fd_h, &nk);
            }
            k = nk;
        }
    }
}

static void ebpf_cerrar(void) {
    ebpf_detener();
    if (g_rb != NULL) {
        ring_buffer__free(g_rb);
        g_rb = NULL;
    }
    if (g_skel != NULL) {
        trace_bpf__destroy(g_skel);
        g_skel = NULL;
    }
}

const fuente_t fuente_ebpf = {
    .abrir        = ebpf_abrir,
    .iniciar      = ebpf_iniciar,
    .iniciar_conn = ebpf_iniciar_conn,
    .leer         = ebpf_leer,
    .detener      = ebpf_detener,
    .cerrar       = ebpf_cerrar
};
