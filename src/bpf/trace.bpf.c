#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

/* Programas eBPF para seguimiento exacto de procesos descendientes por solicitud */

char LICENSE[] SEC("license") = "GPL";

enum ebpf_ev_tipo {
    EV_FORK = 1,
    EV_EXEC = 2,
    EV_EXIT = 3
};

struct ebpf_evento {
    u32 tipo;
    u32 solicitud;
    u32 pid;
    u32 ppid;
    s32 exit_code;
    u64 ts_ns;
    char comm[16];
};

struct conn_info {
    u32 solicitud;
    u32 puerto_backend;
};

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 10240);
    __type(key, u32);
    __type(value, u32);
} marcados SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 10240);
    __type(key, u32);
    __type(value, struct conn_info);
} conexiones SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 10240);
    __type(key, u32);
    __type(value, u32);
} hilos_marcados SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 10240);
    __type(key, u32);
    __type(value, u64);
} cpu SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 10240);
    __type(key, u32);
    __type(value, u64);
} ultima_act SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 10240);
    __type(key, u32);
    __type(value, u64);
} entrada SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 256 * 1024);
} eventos SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, u32);
    __type(value, u64);
} perdidos SEC(".maps");

static __always_inline void registrar_perdido(void) {
    u32 k = 0;
    u64 *cnt = bpf_map_lookup_elem(&perdidos, &k);
    if (cnt) {
        __sync_fetch_and_add(cnt, 1);
    }
}

SEC("tp_btf/sched_process_fork")
int BPF_PROG(on_fork, struct task_struct *parent, struct task_struct *child) {
    u32 p_pid = BPF_CORE_READ(parent, pid);
    u32 c_pid = BPF_CORE_READ(child, pid);
    u32 *hsol = bpf_map_lookup_elem(&hilos_marcados, &p_pid);
    if (hsol) {
        u32 hsv = *hsol;
        bpf_map_update_elem(&hilos_marcados, &c_pid, &hsv, BPF_ANY);
    }

    u32 p_tgid = BPF_CORE_READ(parent, tgid);
    u32 *sol = bpf_map_lookup_elem(&marcados, &p_tgid);
    if (!sol) {
        return 0;
    }

    u32 c_tgid = BPF_CORE_READ(child, tgid);
    if (c_pid != c_tgid) {
        return 0;
    }

    u32 sol_val = *sol;
    bpf_map_update_elem(&marcados, &c_tgid, &sol_val, BPF_ANY);

    struct ebpf_evento *ev = bpf_ringbuf_reserve(&eventos, sizeof(*ev), 0);
    if (!ev) {
        registrar_perdido();
        return 0;
    }
    ev->tipo = EV_FORK;
    ev->solicitud = sol_val;
    ev->pid = c_tgid;
    ev->ppid = p_tgid;
    ev->exit_code = 0;
    ev->ts_ns = bpf_ktime_get_ns();
    BPF_CORE_READ_STR_INTO(&ev->comm, child, comm);
    bpf_ringbuf_submit(ev, 0);
    return 0;
}

SEC("tp_btf/sched_process_exec")
int BPF_PROG(on_exec, struct task_struct *p) {
    u32 tgid = BPF_CORE_READ(p, tgid);
    u32 *sol = bpf_map_lookup_elem(&marcados, &tgid);
    if (!sol) {
        return 0;
    }

    struct ebpf_evento *ev = bpf_ringbuf_reserve(&eventos, sizeof(*ev), 0);
    if (!ev) {
        registrar_perdido();
        return 0;
    }
    ev->tipo = EV_EXEC;
    ev->solicitud = *sol;
    ev->pid = tgid;
    ev->ppid = 0;
    ev->exit_code = 0;
    ev->ts_ns = bpf_ktime_get_ns();
    BPF_CORE_READ_STR_INTO(&ev->comm, p, comm);
    bpf_ringbuf_submit(ev, 0);
    return 0;
}

SEC("tp_btf/sched_process_exit")
int BPF_PROG(on_exit, struct task_struct *p) {
    u32 pid = BPF_CORE_READ(p, pid);
    u32 tgid = BPF_CORE_READ(p, tgid);
    if (pid != tgid) {
        return 0;
    }

    u32 *sol = bpf_map_lookup_elem(&marcados, &tgid);
    if (!sol) {
        return 0;
    }

    u32 sol_val = *sol;
    struct ebpf_evento *ev = bpf_ringbuf_reserve(&eventos, sizeof(*ev), 0);
    if (!ev) {
        registrar_perdido();
    } else {
        ev->tipo = EV_EXIT;
        ev->solicitud = sol_val;
        ev->pid = tgid;
        ev->ppid = 0;
        ev->exit_code = (s32)(BPF_CORE_READ(p, exit_code) >> 8);
        ev->ts_ns = bpf_ktime_get_ns();
        BPF_CORE_READ_STR_INTO(&ev->comm, p, comm);
        bpf_ringbuf_submit(ev, 0);
    }

    bpf_map_delete_elem(&marcados, &tgid);
    return 0;
}

SEC("fexit/tcp_recvmsg")
int BPF_PROG(on_tcp_recvmsg_exit, struct sock *sk, struct msghdr *msg,
             size_t len, int flags, int ret) {
    (void)msg;
    (void)len;
    (void)flags;
    if (ret <= 0 || !sk) {
        return 0;
    }
    u16 family = BPF_CORE_READ(sk, __sk_common.skc_family);
    if (family != 2) {
        return 0;
    }
    u16 dport_be = BPF_CORE_READ(sk, __sk_common.skc_dport);
    u32 rport = (u32)__builtin_bswap16(dport_be);
    struct conn_info *info = bpf_map_lookup_elem(&conexiones, &rport);
    if (!info) {
        return 0;
    }
    u16 lport = BPF_CORE_READ(sk, __sk_common.skc_num);
    u32 daddr = BPF_CORE_READ(sk, __sk_common.skc_daddr);
    u32 saddr = BPF_CORE_READ(sk, __sk_common.skc_rcv_saddr);
    if ((u32)lport != info->puerto_backend || daddr != 0x0100007FU || saddr != 0x0100007FU) {
        return 0;
    }
    u32 tid = (u32)bpf_get_current_pid_tgid();
    u32 sol_val = info->solicitud;
    bpf_map_update_elem(&hilos_marcados, &tid, &sol_val, BPF_ANY);
    u64 now = bpf_ktime_get_ns();
    bpf_map_update_elem(&entrada, &tid, &now, BPF_ANY);
    return 0;
}

SEC("tp_btf/sched_switch")
int BPF_PROG(on_switch, bool preempt, struct task_struct *prev, struct task_struct *next) {
    (void)preempt;
    u64 now = bpf_ktime_get_ns();

    u32 prev_pid = BPF_CORE_READ(prev, pid);
    u64 *t_in = bpf_map_lookup_elem(&entrada, &prev_pid);
    if (t_in) {
        u32 prev_tgid = BPF_CORE_READ(prev, tgid);
        u32 *sol = bpf_map_lookup_elem(&marcados, &prev_tgid);
        if (!sol) {
            sol = bpf_map_lookup_elem(&hilos_marcados, &prev_pid);
        }
        if (sol && now > *t_in) {
            u64 delta = now - *t_in;
            u32 sol_val = *sol;
            u64 *acc = bpf_map_lookup_elem(&cpu, &sol_val);
            if (acc) {
                __sync_fetch_and_add(acc, delta);
            } else {
                bpf_map_update_elem(&cpu, &sol_val, &delta, BPF_ANY);
            }
            bpf_map_update_elem(&ultima_act, &sol_val, &now, BPF_ANY);
        }
        bpf_map_delete_elem(&entrada, &prev_pid);
    }

    u32 next_pid = BPF_CORE_READ(next, pid);
    u32 next_tgid = BPF_CORE_READ(next, tgid);
    if (bpf_map_lookup_elem(&marcados, &next_tgid) ||
        bpf_map_lookup_elem(&hilos_marcados, &next_pid)) {
        bpf_map_update_elem(&entrada, &next_pid, &now, BPF_ANY);
    }
    return 0;
}
