#include "common.h"

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (w == 0) {
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
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        total += (size_t)r;
    }
    return 1;
}

void id_hex(char *out, int bytes) {
    unsigned char buf[64];
    int total = 0;
    if (bytes > (int)sizeof(buf)) {
        bytes = (int)sizeof(buf);
    }
    while (total < bytes) {
        ssize_t r = getrandom(buf + total, (size_t)(bytes - total), 0);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        total += (int)r;
    }
    for (int i = 0; i < bytes; i++) {
        unsigned int byte_val = (i < total) ? buf[i] : 0;
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

int ruta_hermana(const char *nombre, char *out, size_t out_sz) {
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0 || (size_t)n >= sizeof(exe) - 1) {
        return -1;
    }
    exe[n] = '\0';

    char *barra = strrchr(exe, '/');
    if (barra == NULL) {
        return -1;
    }
    *barra = '\0';

    int escrito = snprintf(out, out_sz, "%s/%s", exe, nombre);
    if (escrito < 0 || (size_t)escrito >= out_sz) {
        return -1;
    }
    return 0;
}

int crear_dir_privado(char *out, size_t out_sz) {
    const char *base = getenv("XDG_RUNTIME_DIR");
    if (base == NULL || base[0] == '\0') {
        base = "/tmp";
    }

    int escrito = snprintf(out, out_sz, "%s/tracelite-XXXXXX", base);
    if (escrito < 0 || (size_t)escrito >= out_sz) {
        fprintf(stderr, "[tracelite] Ruta demasiado larga para directorio privado (%s).\n", base);
        return -1;
    }
    if (mkdtemp(out) == NULL) {
        perror("mkdtemp");
        return -1;
    }
    return 0;
}

int validar_ruta_socket(const char *sock_path) {
    struct sockaddr_un dummy;
    if (sock_path == NULL || sock_path[0] == '\0') {
        return -1;
    }
    if (strlen(sock_path) >= sizeof(dummy.sun_path)) {
        fprintf(stderr, "[tracelite] Ruta de socket excede %zu bytes: %s\n",
                sizeof(dummy.sun_path) - 1, sock_path);
        return -1;
    }
    return 0;
}

int crear_servidor_unix(const char *sock_path, int backlog) {
    if (validar_ruta_socket(sock_path) != 0) {
        return -1;
    }
    unlink(sock_path);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, backlog) < 0) {
        close(fd);
        unlink(sock_path);
        return -1;
    }
    return fd;
}

int conectar_unix(const char *sock_path, int intentos, int espera_ms) {
    if (validar_ruta_socket(sock_path) != 0) {
        return -1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);

    for (int i = 0; i < intentos; i++) {
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return -1;
        }
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            return fd;
        }
        close(fd);
        if (i + 1 < intentos && espera_ms > 0) {
            dormir_ms(espera_ms);
        }
    }
    return -1;
}

int aceptar_unix(int server_fd, int timeout_ms) {
    int cfd = accept(server_fd, NULL, NULL);
    if (cfd < 0) {
        return -1;
    }
    int flags = fcntl(cfd, F_GETFD);
    if (flags >= 0) {
        fcntl(cfd, F_SETFD, flags | FD_CLOEXEC);
    }
    if (timeout_ms > 0) {
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    return cfd;
}

void nombre_base_cmd(const char *linea, char *out, size_t out_sz) {
    if (out_sz == 0) {
        return;
    }
    out[0] = '\0';
    if (linea == NULL) {
        return;
    }
    while (*linea == ' ' || *linea == '\t') {
        linea++;
    }
    const char *fin = linea;
    while (*fin != '\0' && *fin != ' ' && *fin != '\t') {
        fin++;
    }
    const char *base = linea;
    for (const char *p = linea; p < fin; p++) {
        if (*p == '/') {
            base = p + 1;
        }
    }
    size_t len = (size_t)(fin - base);
    if (len >= out_sz) {
        len = out_sz - 1;
    }
    memcpy(out, base, len);
    out[len] = '\0';
}

static int es_dir_pid(const char *s) {
    if (s[0] == '\0') return 0;
    for (int i = 0; s[i] != '\0'; i++) {
        if (!isdigit((unsigned char)s[i])) return 0;
    }
    return 1;
}

int resolver_pid_puerto_tcp(int puerto) {
    FILE *f = fopen("/proc/net/tcp", "r");
    if (f == NULL) return -1;

    char linea[512];
    unsigned long inode_obj = 0;
    if (fgets(linea, sizeof(linea), f) != NULL) {
        while (fgets(linea, sizeof(linea), f) != NULL) {
            unsigned int ip = 0, port = 0, st = 0;
            unsigned long inode = 0;
            if (sscanf(linea, " %*d: %x:%x %*x:%*x %x %*s %*s %*s %*d %*d %lu",
                       &ip, &port, &st, &inode) >= 4) {
                if (st == 0x0A && (int)port == puerto &&
                    (ip == 0x0100007F || ip == 0x00000000) && inode > 0) {
                    inode_obj = inode;
                    break;
                }
            }
        }
    }
    fclose(f);
    if (inode_obj == 0) return -1;

    char buscado[64];
    snprintf(buscado, sizeof(buscado), "socket:[%lu]", inode_obj);

    DIR *d = opendir("/proc");
    if (d == NULL) return -1;
    struct dirent *ent;
    int pid_hallado = -1;

    while ((ent = readdir(d)) != NULL && pid_hallado < 0) {
        if (!es_dir_pid(ent->d_name)) continue;
        char dir_fd[300];
        snprintf(dir_fd, sizeof(dir_fd), "/proc/%s/fd", ent->d_name);
        DIR *dfd = opendir(dir_fd);
        if (dfd == NULL) continue;

        struct dirent *fent;
        while ((fent = readdir(dfd)) != NULL) {
            char ruta_fd[384], destino[128];
            int nr = snprintf(ruta_fd, sizeof(ruta_fd), "%s/%s", dir_fd, fent->d_name);
            if (nr < 0 || (size_t)nr >= sizeof(ruta_fd)) continue;
            ssize_t n = readlink(ruta_fd, destino, sizeof(destino) - 1);
            if (n > 0) {
                destino[n] = '\0';
                if (strcmp(destino, buscado) == 0) {
                    pid_hallado = atoi(ent->d_name);
                    break;
                }
            }
        }
        closedir(dfd);
    }
    closedir(d);
    return pid_hallado;
}

int resolver_pid_unidad(const char *unidad) {
    if (unidad == NULL || unidad[0] == '\0') return -1;
    int p[2];
    if (pipe2(p, O_CLOEXEC) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        return -1;
    }
    if (pid == 0) {
        close(p[0]);
        dup2(p[1], STDOUT_FILENO);
        close(p[1]);
        char *args[6] = { "systemctl", "show", "-p", "MainPID", "--value", (char *)unidad };
        char *argv_exec[7] = { args[0], args[1], args[2], args[3], args[4], args[5], NULL };
        execvp("systemctl", argv_exec);
        _exit(127);
    }
    close(p[1]);
    char buf[64] = {0};
    ssize_t n = read(p[0], buf, sizeof(buf) - 1);
    close(p[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    if (n <= 0) return -1;
    int mpid = atoi(buf);
    return (mpid > 0) ? mpid : 0;
}

int resolver_pid_nombre_proceso(const char *nombre) {
    if (nombre == NULL || nombre[0] == '\0') return -1;
    if (es_dir_pid(nombre)) return atoi(nombre);

    DIR *d = opendir("/proc");
    if (d == NULL) return -1;
    struct dirent *ent;
    int mejor_pid = -1;
    int mi_pid = (int)getpid();

    while ((ent = readdir(d)) != NULL) {
        if (!es_dir_pid(ent->d_name)) continue;
        int pid = atoi(ent->d_name);
        if (pid <= 2 || pid == mi_pid) continue;

        char ruta[64], comm[64] = {0};
        snprintf(ruta, sizeof(ruta), "/proc/%d/comm", pid);
        FILE *f = fopen(ruta, "r");
        if (f == NULL) continue;
        if (fgets(comm, sizeof(comm), f) != NULL) {
            size_t len = strlen(comm);
            if (len > 0 && comm[len - 1] == '\n') comm[len - 1] = '\0';
            if (strcasecmp(comm, nombre) == 0 || strcasestr(comm, nombre) != NULL) {
                mejor_pid = pid;
                fclose(f);
                break;
            }
        }
        fclose(f);
    }
    closedir(d);
    return mejor_pid;
}
