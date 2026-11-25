#include "almacen.h"
#include "tabla.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Persistencia diaria JSONL append-only con retención por segmento */

static int crear_dirs_recursivo(const char *ruta) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", ruta);
    size_t len = strlen(tmp);
    if (len == 0) return -1;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

int almacen_resolver_dir(char *out, size_t out_sz) {
    const char *env = getenv("TRACELITE_DIR");
    if (env != NULL && env[0] != '\0') {
        int n = snprintf(out, out_sz, "%s", env);
        return (n > 0 && (size_t)n < out_sz) ? 0 : -1;
    }
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        home = "/tmp";
    }
    int n = snprintf(out, out_sz, "%s/.local/share/tracelite", home);
    return (n > 0 && (size_t)n < out_sz) ? 0 : -1;
}

static int es_nombre_segmento_jsonl(const char *nombre) {
    if (strlen(nombre) != 16) return 0;
    for (int i = 0; i < 4; i++) if (!isdigit((unsigned char)nombre[i])) return 0;
    if (nombre[4] != '-') return 0;
    for (int i = 5; i < 7; i++) if (!isdigit((unsigned char)nombre[i])) return 0;
    if (nombre[7] != '-') return 0;
    for (int i = 8; i < 10; i++) if (!isdigit((unsigned char)nombre[i])) return 0;
    return strcmp(nombre + 10, ".jsonl") == 0;
}

static int cmp_nombres(const void *a, const void *b) {
    const char *sa = *(const char *const *)a;
    const char *sb = *(const char *const *)b;
    return strcmp(sa, sb);
}

static void escribir_json_str(FILE *f, const char *s) {
    fputc('"', f);
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    while (*p != '\0') {
        unsigned char c = *p;
        if (c == '"') {
            fputs("\\\"", f);
            p++;
        } else if (c == '\\') {
            fputs("\\\\", f);
            p++;
        } else if (c == '\n') {
            fputs("\\n", f);
            p++;
        } else if (c == '\r') {
            fputs("\\r", f);
            p++;
        } else if (c == '\t') {
            fputs("\\t", f);
            p++;
        } else if (c < 0x20) {
            fprintf(f, "\\u%04x", c);
            p++;
        } else if (c < 0x80) {
            fputc((int)c, f);
            p++;
        } else {
            /* Validar secuencia UTF-8 básica; sustituir bytes inválidos por '?' */
            int seq = 0;
            if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) seq = 2;
            else if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) seq = 3;
            else if ((c & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) seq = 4;

            if (seq > 0) {
                for (int i = 0; i < seq; i++) fputc((int)p[i], f);
                p += seq;
            } else {
                fputc('?', f);
                p++;
            }
        }
    }
    fputc('"', f);
}

static int extraer_str(const char *linea, const char *clave, char *out, size_t out_sz) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":\"", clave);
    const char *p = strstr(linea, pat);
    if (p == NULL) return -1;
    p += strlen(pat);

    size_t pos = 0;
    while (*p != '\0' && *p != '"') {
        char ch = *p;
        if (ch == '\\' && p[1] != '\0') {
            p++;
            if (*p == 'n') ch = '\n';
            else if (*p == 'r') ch = '\r';
            else if (*p == 't') ch = '\t';
            else ch = *p;
        }
        if (pos + 1 < out_sz) {
            out[pos++] = ch;
        }
        p++;
    }
    if (*p != '"') return -1;
    out[pos] = '\0';
    return 0;
}

static int extraer_i64(const char *linea, const char *clave, int64_t *out) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", clave);
    const char *p = strstr(linea, pat);
    if (p == NULL) return -1;
    p += strlen(pat);
    char *end = NULL;
    long long v = strtoll(p, &end, 10);
    if (end == p) return -1;
    *out = (int64_t)v;
    return 0;
}

static int extraer_dbl(const char *linea, const char *clave, double *out) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", clave);
    const char *p = strstr(linea, pat);
    if (p == NULL) return -1;
    p += strlen(pat);
    char *end = NULL;
    double v = strtod(p, &end);
    if (end == p) return -1;
    *out = v;
    return 0;
}

static int parsear_linea_jsonl(const char *linea, almacen_registro_t *reg) {
    size_t len = strlen(linea);
    if (len < 2 || linea[0] != '{') return -1;
    const char *fin = linea + len - 1;
    while (fin > linea && (*fin == '\n' || *fin == '\r' || *fin == ' ')) fin--;
    if (*fin != '}') return -1;

    memset(reg, 0, sizeof(*reg));
    int64_t v = 0, plazo = 0, sol = 0, estado = 0, pid = 0, n_procs = 0;
    if (extraer_i64(linea, "v", &v) != 0 || v != 1) return -1;
    if (extraer_str(linea, "trace_id", reg->span.trace_id, sizeof(reg->span.trace_id)) != 0) return -1;
    if (extraer_str(linea, "span_id", reg->span.span_id, sizeof(reg->span.span_id)) != 0) return -1;
    if (extraer_str(linea, "parent", reg->span.parent, sizeof(reg->span.parent)) != 0) return -1;
    if (extraer_str(linea, "servicio", reg->span.servicio, sizeof(reg->span.servicio)) != 0) return -1;
    if (extraer_str(linea, "operacion", reg->span.operacion, sizeof(reg->span.operacion)) != 0) return -1;
    if (extraer_i64(linea, "ini_ns", &reg->span.ini) != 0) return -1;
    if (extraer_i64(linea, "fin_ns", &reg->span.fin) != 0) return -1;
    if (extraer_i64(linea, "ini_real_ns", &reg->meta.ini_real_ns) != 0) return -1;
    if (extraer_i64(linea, "fin_real_ns", &reg->meta.fin_real_ns) != 0) return -1;
    if (extraer_dbl(linea, "dur_ms", &reg->span.dur) != 0) return -1;
    if (extraer_i64(linea, "estado", &estado) != 0) return -1;

    reg->span.estado = (int)estado;
    extraer_str(linea, "tipo", reg->meta.tipo, sizeof(reg->meta.tipo));
    extraer_str(linea, "modo", reg->meta.modo, sizeof(reg->meta.modo));
    extraer_str(linea, "destino", reg->meta.destino, sizeof(reg->meta.destino));
    extraer_str(linea, "tiempo", reg->meta.tiempo, sizeof(reg->meta.tiempo));
    if (extraer_i64(linea, "plazo_s", &plazo) == 0) reg->meta.plazo_s = (int)plazo;
    if (extraer_i64(linea, "solicitud", &sol) == 0) reg->meta.solicitud = (int)sol;
    extraer_str(linea, "msg", reg->span.msg, sizeof(reg->span.msg));
    extraer_str(linea, "anomalia", reg->span.anomalia, sizeof(reg->span.anomalia));
    if (extraer_i64(linea, "pid", &pid) == 0) reg->span.pid = (int)pid;
    extraer_dbl(linea, "cpu_ms", &reg->span.cpu_ms);
    if (extraer_i64(linea, "n_procs", &n_procs) == 0) reg->span.n_procs = (int)n_procs;
    extraer_str(linea, "atribucion", reg->span.atribucion, sizeof(reg->span.atribucion));
    return 0;
}

static void agregar_a_indice(almacen_indice_t *idx, const almacen_registro_t *reg) {
    if (idx->n >= MAX_ALMACEN_SPANS) {
        memmove(&idx->regs[0], &idx->regs[1],
                (size_t)(idx->n - 1) * sizeof(almacen_registro_t));
        idx->n--;
    }
    if (idx->n >= idx->cap) {
        int ncap = (idx->cap == 0) ? 64 : idx->cap * 2;
        if (ncap > MAX_ALMACEN_SPANS) ncap = MAX_ALMACEN_SPANS;
        almacen_registro_t *nuevo = (almacen_registro_t *)realloc(
            idx->regs, (size_t)ncap * sizeof(almacen_registro_t));
        if (nuevo == NULL) return;
        idx->regs = nuevo;
        idx->cap = ncap;
    }
    idx->regs[idx->n++] = *reg;
}

static int64_t calcular_corte_ns(int ret_dias, int ret_seg) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int64_t ahora_real = (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
    int64_t ttl_ns = 0;
    if (ret_seg > 0) {
        ttl_ns = (int64_t)ret_seg * 1000000000LL;
    } else if (ret_dias > 0) {
        ttl_ns = (int64_t)ret_dias * 86400LL * 1000000000LL;
    } else {
        return 0;
    }
    return ahora_real - ttl_ns;
}

void almacen_aplicar_retencion(almacen_indice_t *idx, int ret_dias, int ret_seg) {
    int64_t corte_ns = calcular_corte_ns(ret_dias, ret_seg);
    if (corte_ns <= 0) return;

    int w = 0;
    for (int i = 0; i < idx->n; i++) {
        if (idx->regs[i].meta.fin_real_ns >= corte_ns) {
            idx->regs[w++] = idx->regs[i];
        }
    }
    idx->n = w;

    DIR *d = opendir(idx->dir);
    if (d == NULL) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!es_nombre_segmento_jsonl(ent->d_name)) continue;

        char ruta[PATH_MAX];
        int nr = snprintf(ruta, sizeof(ruta), "%s/%s", idx->dir, ent->d_name);
        if (nr < 0 || (size_t)nr >= sizeof(ruta)) continue;

        FILE *f = fopen(ruta, "r");
        if (f == NULL) continue;

        int tiene_vigente = 0;
        int tiene_alguno = 0;
        char buf[2048];
        while (fgets(buf, sizeof(buf), f) != NULL) {
            almacen_registro_t reg;
            if (parsear_linea_jsonl(buf, &reg) == 0) {
                tiene_alguno = 1;
                if (reg.meta.fin_real_ns >= corte_ns) {
                    tiene_vigente = 1;
                    break;
                }
            }
        }
        fclose(f);

        if (tiene_alguno && !tiene_vigente) {
            unlink(ruta);
        }
    }
    closedir(d);
}

int almacen_cargar(almacen_indice_t *idx, const char *dir_opc, int ret_dias, int ret_seg) {
    memset(idx, 0, sizeof(*idx));
    if (dir_opc != NULL && dir_opc[0] != '\0') {
        snprintf(idx->dir, sizeof(idx->dir), "%s", dir_opc);
    } else if (almacen_resolver_dir(idx->dir, sizeof(idx->dir)) != 0) {
        return -1;
    }

    if (crear_dirs_recursivo(idx->dir) != 0) {
        return -1;
    }

    DIR *d = opendir(idx->dir);
    if (d == NULL) return -1;

    char **nombres = NULL;
    int n_arch = 0, cap_arch = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!es_nombre_segmento_jsonl(ent->d_name)) continue;
        if (n_arch >= cap_arch) {
            int nc = (cap_arch == 0) ? 16 : cap_arch * 2;
            char **nuevo = (char **)realloc(nombres, (size_t)nc * sizeof(char *));
            if (nuevo == NULL) break;
            nombres = nuevo;
            cap_arch = nc;
        }
        nombres[n_arch++] = strdup(ent->d_name);
    }
    closedir(d);

    if (n_arch > 1) {
        qsort(nombres, (size_t)n_arch, sizeof(char *), cmp_nombres);
    }

    int64_t corte_ns = calcular_corte_ns(ret_dias, ret_seg);
    for (int i = 0; i < n_arch; i++) {
        char ruta[PATH_MAX];
        int nr = snprintf(ruta, sizeof(ruta), "%s/%s", idx->dir, nombres[i]);
        free(nombres[i]);
        if (nr < 0 || (size_t)nr >= sizeof(ruta)) continue;

        FILE *f = fopen(ruta, "r");
        if (f == NULL) continue;
        char buf[2048];
        while (fgets(buf, sizeof(buf), f) != NULL) {
            if (buf[0] == '\n' || buf[0] == '\0') continue;
            almacen_registro_t reg;
            if (parsear_linea_jsonl(buf, &reg) == 0) {
                if (corte_ns <= 0 || reg.meta.fin_real_ns >= corte_ns) {
                    agregar_a_indice(idx, &reg);
                }
            } else {
                idx->corruptas++;
            }
        }
        fclose(f);
    }
    free(nombres);

    almacen_aplicar_retencion(idx, ret_dias, ret_seg);
    return 0;
}

int almacen_guardar_span(almacen_indice_t *idx, const span_t *s, const almacen_meta_t *meta) {
    if (idx->dir[0] == '\0') return -1;

    time_t seg = (time_t)(meta->fin_real_ns / 1000000000LL);
    struct tm tm_info;
    localtime_r(&seg, &tm_info);

    char nombre[32];
    strftime(nombre, sizeof(nombre), "%Y-%m-%d.jsonl", &tm_info);

    char ruta[PATH_MAX];
    int nr = snprintf(ruta, sizeof(ruta), "%s/%s", idx->dir, nombre);
    if (nr < 0 || (size_t)nr >= sizeof(ruta)) return -1;

    FILE *f = fopen(ruta, "a");
    if (f == NULL) return -1;

    fprintf(f, "{\"v\":1,\"tipo\":");
    escribir_json_str(f, meta->tipo);
    fprintf(f, ",\"modo\":");
    escribir_json_str(f, meta->modo);
    fprintf(f, ",\"destino\":");
    escribir_json_str(f, meta->destino);
    fprintf(f, ",\"tiempo\":");
    escribir_json_str(f, meta->tiempo);
    fprintf(f, ",\"plazo_s\":%d,\"solicitud\":%d,\"trace_id\":",
            meta->plazo_s, meta->solicitud);
    escribir_json_str(f, s->trace_id);
    fprintf(f, ",\"span_id\":");
    escribir_json_str(f, s->span_id);
    fprintf(f, ",\"parent\":");
    escribir_json_str(f, s->parent);
    fprintf(f, ",\"servicio\":");
    escribir_json_str(f, s->servicio);
    fprintf(f, ",\"operacion\":");
    escribir_json_str(f, s->operacion);
    fprintf(f, ",\"ini_ns\":%lld,\"fin_ns\":%lld,\"ini_real_ns\":%lld,\"fin_real_ns\":%lld,"
               "\"dur_ms\":%.3f,\"estado\":%d,\"msg\":",
            (long long)s->ini, (long long)s->fin,
            (long long)meta->ini_real_ns, (long long)meta->fin_real_ns,
            s->dur, s->estado);
    escribir_json_str(f, s->msg);
    fprintf(f, ",\"anomalia\":");
    escribir_json_str(f, s->anomalia);
    fprintf(f, ",\"pid\":%d,\"cpu_ms\":%.3f,\"n_procs\":%d,\"atribucion\":",
            s->pid, s->cpu_ms, s->n_procs);
    escribir_json_str(f, s->atribucion);
    fprintf(f, "}\n");

    fflush(f);
    fsync(fileno(f));
    fclose(f);

    almacen_registro_t reg;
    reg.meta = *meta;
    reg.span = *s;
    agregar_a_indice(idx, &reg);
    return 0;
}

void almacen_liberar(almacen_indice_t *idx) {
    free(idx->regs);
    idx->regs = NULL;
    idx->n = 0;
    idx->cap = 0;
}

int almacen_mostrar_historial(const almacen_indice_t *idx, const char *trace_id_filtro) {
    if (idx->corruptas > 0) {
        fprintf(stderr, "[almacen] Aviso: %d líneas corruptas ignoradas.\n", idx->corruptas);
    }
    if (idx->n == 0) {
        printf("-- Historial vacío (%s) --\n", idx->dir);
        return 0;
    }

    span_t *lista = (span_t *)malloc(sizeof(span_t) * (size_t)idx->n);
    if (lista == NULL) return 1;

    int count = 0;
    for (int i = 0; i < idx->n; i++) {
        if (trace_id_filtro != NULL &&
            strcmp(idx->regs[i].span.trace_id, trace_id_filtro) != 0) {
            continue;
        }
        lista[count++] = idx->regs[i].span;
    }

    if (count == 0) {
        printf("-- No se encontró trace_id %s en el historial --\n", trace_id_filtro);
        free(lista);
        return 1;
    }

    int num_trazas = imprimir_trazas_spans(lista, count);
    imprimir_resumen_spans(lista, count, num_trazas);
    free(lista);
    return 0;
}
