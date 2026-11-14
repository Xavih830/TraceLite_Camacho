#include "common.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Opción 3 (con descubrimiento recursivo de jerarquía por PPID):
 * - Escanea /proc/<pid>/stat para obtener el árbol real (PID, PPID, comm).
 * - Parte de un PID raíz (pasado por argv[1], o autoseleccionado buscando un
 *   proceso vivo con subprocesos) y descubre recursivamente sus hijos y nietos.
 * - Lanza collector_sock y un agente monitor_pid por cada nodo del árbol
 *   mediante fork() + execv(), preservando la relación padre-hijo real.
 */

#define MAX_PROCS_SO 1024
#define MAX_NODOS    20

typedef struct {
    int  pid;
    int  ppid;
    char comm[32];
} proc_info_t;

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
    if (sscanf(rparen + 2, "%*c %d", &ppid) != 1) return -1;

    info->pid = pid;
    info->ppid = ppid;
    return 0;
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
            if (strcmp(info.comm, "orquestador_pid") != 0 &&
                strcmp(info.comm, "collector_sock") != 0 &&
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

static int elegir_pid_raiz_automatico(const proc_info_t *lista, int n) {
    /* Priorizar algún proceso que tenga hijos reales y no sea el kernel/PID 1 si hay otro mejor */
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

    /* Si no hay otros con hijos, usar PID 2 o PID 1 si está accesible */
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

int main(int argc, char *argv[]) {
    const char *sock_path = "/tmp/tracelite_op3.sock";

    proc_info_t *lista = (proc_info_t *)malloc(sizeof(proc_info_t) * MAX_PROCS_SO);
    if (lista == NULL) return 1;

    int n_procs = cargar_procesos_so(lista, MAX_PROCS_SO, (int)getpid());
    if (n_procs == 0) {
        fprintf(stderr, "[orquestador] No se encontraron procesos accesibles en /proc.\n");
        free(lista);
        return 1;
    }

    int pid_raiz = (argc > 1) ? atoi(argv[1]) : elegir_pid_raiz_automatico(lista, n_procs);
    if (pid_raiz <= 0) {
        fprintf(stderr, "[orquestador] PID raíz inválido.\n");
        free(lista);
        return 1;
    }

    /* 1. Lanzar collector_sock con fork() + execv() */
    fflush(stdout);
    pid_t pid_col = fork();
    if (pid_col < 0) {
        perror("fork collector");
        free(lista);
        return 1;
    }
    if (pid_col == 0) {
        char *args_col[3] = { "./collector_sock", (char *)sock_path, NULL };
        execv("./collector_sock", args_col);
        perror("execv collector_sock");
        _exit(127);
    }

    /* 2. Crear span raíz del orquestador y construir el árbol real de PIDs */
    span_t raiz;
    span_ini(&raiz, NULL, NULL, "orquestador", "arbol_ppid_so");

    nodo_obs_t nodos[MAX_NODOS];
    int n_nodos = 0;
    construir_subarbol(lista, n_procs, pid_raiz, raiz.span_id, nodos, &n_nodos, 0);
    free(lista);

    if (n_nodos == 0) {
        fprintf(stderr, "[orquestador] El PID %d no está en /proc o no es accesible.\n", pid_raiz);
        span_fin(&raiz, 1, "pid_no_encontrado", "");
    } else {
        printf("[orquestador] Árbol descubierto desde PID raíz %d (%d procesos):\n",
               pid_raiz, n_nodos);
        for (int i = 0; i < n_nodos; i++) {
            printf("  - PID %-6d (%s)\n", nodos[i].pid, nodos[i].comm);
        }
    }

    /* 3. Lanzar un monitor_pid en paralelo por cada proceso del árbol */
    pid_t hijos[MAX_NODOS];
    for (int i = 0; i < n_nodos; i++) {
        fflush(stdout);
        hijos[i] = fork();
        if (hijos[i] == 0) {
            char pid_str[16];
            snprintf(pid_str, sizeof(pid_str), "%d", nodos[i].pid);
            char *args_mon[8] = {
                "./monitor_pid",
                pid_str,
                raiz.trace_id,
                nodos[i].span_id,
                nodos[i].parent_span_id,
                (char *)sock_path,
                "250",
                NULL
            };
            execv("./monitor_pid", args_mon);
            perror("execv monitor_pid");
            _exit(127);
        }
    }

    for (int i = 0; i < n_nodos; i++) {
        if (hijos[i] > 0) {
            waitpid(hijos[i], NULL, 0);
        }
    }

    if (n_nodos > 0) {
        span_fin(&raiz, 0, "", "");
    }

    int sock_fd = conectar_colector(sock_path);
    if (sock_fd >= 0) {
        escribir_exacto(sock_fd, &raiz, sizeof(span_t));

        span_t fin;
        memset(&fin, 0, sizeof(fin));
        snprintf(fin.servicio, sizeof(fin.servicio), "__FIN__");
        escribir_exacto(sock_fd, &fin, sizeof(span_t));
        close(sock_fd);
    }

    waitpid(pid_col, NULL, 0);
    return 0;
}
