#!/usr/bin/env bash
set -euo pipefail

RAIZ="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$RAIZ/bin/tracelite"

TMP_DIR="$(mktemp -d /tmp/tl_f7_XXXX)"
python3 "$RAIZ/docs/pruebas/backend_demo.py" 18085 &
BE_PID=$!
trap 'kill $BE_PID 2>/dev/null || true; rm -rf "$TMP_DIR"' EXIT
sleep 0.3

export TRACELITE_DIR="$TMP_DIR"
export XDG_RUNTIME_DIR="$TMP_DIR"

echo "[1/3] /espera?ms=300 con --fuente ebpf..."
"$BIN" --fuente ebpf --linea-base 0 --backend 127.0.0.1:18085 --ruta "/espera?ms=300" > /dev/null

echo "[2/3] /hash?mb=64 con --fuente ebpf..."
"$BIN" --fuente ebpf --linea-base 0 --backend 127.0.0.1:18085 --ruta "/hash?mb=64" > /dev/null

echo "[3/3] Prueba de ruido (conexion vs ventana)..."
(while true; do curl -s -o /dev/null "http://127.0.0.1:18085/hash?mb=16" || break; done) &
RUIDO_PID=$!
trap 'kill $RUIDO_PID $BE_PID 2>/dev/null || true; rm -rf "$TMP_DIR"' EXIT
sleep 0.2

"$BIN" --fuente ebpf --linea-base 0 --backend 127.0.0.1:18085 --ruta "/espera?ms=300" > /dev/null
"$BIN" --fuente proc --linea-base 0 --backend 127.0.0.1:18085 --ruta "/espera?ms=300" > /dev/null

kill $RUIDO_PID 2>/dev/null || true
wait $RUIDO_PID 2>/dev/null || true

python3 - "$TMP_DIR" <<'PY'
import glob, json, sys
rows = []
for fn in glob.glob(sys.argv[1] + "/*.jsonl"):
    with open(fn) as f:
        for line in f:
            d = json.loads(line)
            if d["servicio"] == "servicio_b":
                rows.append(d)
for i, d in enumerate(rows, 1):
    print(f"  req {i}: op={d['operacion']} atribucion={d['atribucion']} dur_ms={d['dur_ms']:.2f} cpu_ms={d['cpu_ms']:.2f}")
assert rows[0]["atribucion"] == "conexion" and rows[0]["cpu_ms"] < 20.0, rows[0]
assert rows[1]["atribucion"] == "conexion", rows[1]
PY
echo "[OK] test_ebpf_conexion.sh superado"
