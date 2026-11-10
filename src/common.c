#include "common.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * Inicializa la semilla aleatoria una vez por proceso (PID) usando
 * la alternativa aprobada en la sección 0.1: srand(getpid() ^ time(NULL)).
 */
static void asegurar_semilla(void) {
    static int pid_inicializado = 0;
    int pid_actual = (int)getpid();
    if (pid_inicializado != pid_actual) {
        srand((unsigned int)(pid_actual ^ (int)time(NULL)));
        pid_inicializado = pid_actual;
    }
}

int64_t ahora_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

void dormir_ms(int ms) {
    if (ms <= 0) {
        return;
    }
    struct timespec req;
    req.tv_sec  = ms / 1000;
    req.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&req, NULL);
}

int escribir_exacto(int fd, const void *buf, size_t n) {
    const char *p = (const char *)buf;
    size_t total = 0;
    while (total < n) {
        ssize_t w = write(fd, p + total, n - total);
        if (w <= 0) {
            return -1;
        }
        total += (size_t)w;
    }
    return 1;
}

int leer_exacto(int fd, void *buf, size_t n) {
    char *p = (char *)buf;
    size_t total = 0;
    while (total < n) {
        ssize_t r = read(fd, p + total, n - total);
        if (r == 0) {
            return (total == 0) ? 0 : -1;
        }
        if (r < 0) {
            return -1;
        }
        total += (size_t)r;
    }
    return 1;
}

void id_hex(char *out, int bytes) {
    asegurar_semilla();
    for (int i = 0; i < bytes; i++) {
        unsigned int byte_val = (unsigned int)(rand() & 0xFF);
        snprintf(out + (i * 2), 3, "%02x", byte_val);
    }
    out[bytes * 2] = '\0';
}

void span_ini(span_t *s, const char *trace_id, const char *parent,
              const char *servicio, const char *operacion) {
    assert(sizeof(span_t) <= 512);
    memset(s, 0, sizeof(*s));

    if (trace_id != NULL && trace_id[0] != '\0') {
        snprintf(s->trace_id, sizeof(s->trace_id), "%s", trace_id);
    } else {
        id_hex(s->trace_id, 16);
    }

    id_hex(s->span_id, 8);

    if (parent != NULL && parent[0] != '\0') {
        snprintf(s->parent, sizeof(s->parent), "%s", parent);
    } else {
        s->parent[0] = '\0';
    }

    if (servicio != NULL) {
        snprintf(s->servicio, sizeof(s->servicio), "%s", servicio);
    }
    if (operacion != NULL) {
        snprintf(s->operacion, sizeof(s->operacion), "%s", operacion);
    }

    s->pid = (int)getpid();
    s->ini = ahora_ns();
}

void span_fin(span_t *s, int estado, const char *msg, const char *anomalia) {
    s->fin    = ahora_ns();
    s->dur    = (double)(s->fin - s->ini) / 1000000.0;
    s->estado = estado;

    if (msg != NULL) {
        snprintf(s->msg, sizeof(s->msg), "%s", msg);
    } else {
        s->msg[0] = '\0';
    }

    if (anomalia != NULL) {
        snprintf(s->anomalia, sizeof(s->anomalia), "%s", anomalia);
    } else {
        s->anomalia[0] = '\0';
    }
}
