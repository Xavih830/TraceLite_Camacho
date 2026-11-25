#!/usr/bin/env bash
set -u

echo "=== Diagnóstico eBPF (sin sudo) ==="
echo "Kernel: $(uname -r) ($(uname -m))"

if [ -r /sys/kernel/btf/vmlinux ]; then
    echo "BTF: disponible (/sys/kernel/btf/vmlinux, $(stat -c %s /sys/kernel/btf/vmlinux) bytes)"
else
    echo "BTF: NO disponible o no legible"
fi

if [ -r /proc/sys/kernel/unprivileged_bpf_disabled ]; then
    echo "unprivileged_bpf_disabled: $(cat /proc/sys/kernel/unprivileged_bpf_disabled)"
else
    echo "unprivileged_bpf_disabled: no legible"
fi

for cmd in clang llc bpftool setcap getcap; do
    if command -v "$cmd" >/dev/null 2>&1; then
        ver=$("$cmd" --version 2>&1 | head -n 1 || true)
        echo "$cmd: $(command -v "$cmd") ($ver)"
    else
        echo "$cmd: NO encontrado"
    fi
done

if pkg-config --exists libbpf 2>/dev/null; then
    echo "libbpf (pkg-config): $(pkg-config --modversion libbpf)"
elif [ -f /usr/include/bpf/libbpf.h ]; then
    echo "libbpf: cabecera presente en /usr/include/bpf/libbpf.h"
else
    echo "libbpf: NO encontrado"
fi

if [ -r /proc/kallsyms ]; then
    if grep -q " tcp_recvmsg$" /proc/kallsyms 2>/dev/null; then
        echo "kallsyms tcp_recvmsg: presente"
    else
        echo "kallsyms tcp_recvmsg: no encontrado o direcciones ocultas"
    fi
fi

exit 0
