#!/usr/bin/env bash
set -euo pipefail

RAIZ="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="$RAIZ/bin/tracelite"

TMP_DIR="$(mktemp -d /tmp/tl_cmp_XXXX)"
trap 'rm -rf "$TMP_DIR"' EXIT
export TRACELITE_DIR="$TMP_DIR"
export XDG_RUNTIME_DIR="$TMP_DIR"

cat > "$TMP_DIR/cincuenta.c" <<'EOF'
#include <sys/wait.h>
#include <unistd.h>
int main(void) {
    for (int i = 0; i < 50; i++) {
        pid_t p = fork();
        if (p == 0) _exit(0);
        if (p > 0) waitpid(p, NULL, 0);
    }
    return 0;
}
EOF
gcc -O2 "$TMP_DIR/cincuenta.c" -o "$TMP_DIR/cincuenta"

echo "=== Comparando fuentes con 50 hijos efímeros ==="
"$BIN" --fuente proc -- "$TMP_DIR/cincuenta" > /dev/null
"$BIN" --fuente ebpf -- "$TMP_DIR/cincuenta" > /dev/null

python3 - "$TMP_DIR" <<'PY'
import glob, json, sys
files = glob.glob(sys.argv[1] + "/*.jsonl")
for fn in files:
    with open(fn) as f:
        for line in f:
            d = json.loads(line)
            if d["servicio"] == "servicio_b":
                print(f"atribucion={d['atribucion']:8s} n_procs={d['n_procs']:3d} cpu_ms={d['cpu_ms']:.3f} dur_ms={d['dur_ms']:.3f} msg={d['msg']}")
PY
