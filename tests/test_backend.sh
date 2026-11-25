#!/usr/bin/env bash
set -euo pipefail

RAIZ="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$RAIZ/bin/tracelite"

TMP_DIR="$(mktemp -d /tmp/tl_f6_XXXX)"
trap 'rm -rf "$TMP_DIR"' EXIT
export TRACELITE_DIR="$TMP_DIR"
export XDG_RUNTIME_DIR="$TMP_DIR"

echo "[1/4] Backend nuevo (--lanzar-backend)..."
OUT1=$("$BIN" --resumen --fuente proc --linea-base 1 --lanzar-backend "python3 $RAIZ/docs/pruebas/backend_demo.py 18081" --ruta "/hash?mb=4")
echo "$OUT1" | grep -q "solicitudes=5 spans=10"
! pgrep -f "backend_demo.py 18081" >/dev/null 2>&1
echo "  OK"

echo "[2/4] Backend vivo (--backend 127.0.0.1:18082) y traceparent..."
python3 "$RAIZ/docs/pruebas/backend_demo.py" 18082 --mostrar-cabeceras 2> "$TMP_DIR/be.log" &
BE_PID=$!
trap 'kill $BE_PID 2>/dev/null || true; rm -rf "$TMP_DIR"' EXIT
sleep 0.3

OUT2=$("$BIN" --resumen --fuente proc --linea-base 1 --backend 127.0.0.1:18082 --ruta "/hash?mb=4")
echo "$OUT2" | grep -q "solicitudes=5 spans=10"
kill -0 "$BE_PID"
curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:18082/espera?ms=10 | grep -q "200"
grep -q "traceparent=00-" "$TMP_DIR/be.log"
echo "  OK"

echo "[3/4] /error -> http_500..."
OUT3=$("$BIN" --fuente proc --linea-base 0 --backend 127.0.0.1:18082 --ruta "/error")
echo "$OUT3" | grep -q "ERROR"
grep -q "http_500" "$TMP_DIR"/*.jsonl
echo "  OK"

echo "[4/4] Puerto cerrado -> ERROR sin colgarse..."
OUT4=$("$BIN" --fuente proc --linea-base 0 --backend 127.0.0.1:18099 --ruta "/hash?mb=1")
echo "$OUT4" | grep -q "ERROR"
grep -q "conexion_rechazada" "$TMP_DIR"/*.jsonl
echo "  OK"

kill "$BE_PID" 2>/dev/null || true
wait "$BE_PID" 2>/dev/null || true
echo "[OK] test_backend.sh superado con exito"
