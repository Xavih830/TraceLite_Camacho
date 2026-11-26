#!/usr/bin/env bash
set -euo pipefail

# Pruebas integrales enfocadas únicamente en aplicaciones vivas del SO
RAIZ="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="$RAIZ/bin/tracelite"

TMP_DIR="$(mktemp -d /tmp/tl_vivos_XXXX)"
LIVE_PID=""
limpiar() {
    [ -n "$LIVE_PID" ] && kill "$LIVE_PID" 2>/dev/null || true
    rm -rf "$TMP_DIR"
}
trap limpiar EXIT
export XDG_RUNTIME_DIR="$TMP_DIR"

python3 -c 'import subprocess, time; [ (subprocess.Popen(["sleep", "0.35"]).wait(), time.sleep(0.2)) for _ in range(40) ]' &
LIVE_PID=$!
sleep 0.15

echo "=== [1/4] Observación continua de app viva (--resumen -d 2) ==="
OUT1=$(TRACELITE_DIR="$TMP_DIR/c1" "$BIN" --resumen -d 2 --interval 100 "$LIVE_PID")
echo "$OUT1"
echo "$OUT1" | grep -q "RELACION"
echo "$OUT1" | grep -q "orquestador ->"

echo ""
echo "=== [2/4] Modo respuesta + --slo-ms sobre app viva ==="
OUT2=$(TRACELITE_DIR="$TMP_DIR/c2" "$BIN" --resumen --app "$LIVE_PID" --fin orden --linea-base 0 --modo respuesta --slo-ms 50 --entradas "0.15,0.15" -- sleep)
echo "$OUT2"
echo "$OUT2" | grep -q "slo_excedido"

echo ""
echo "=== [3/4] Modo estabilidad sobre app viva (muestreo paralelo del árbol) ==="
OUT3=$(TRACELITE_DIR="$TMP_DIR/c3" "$BIN" --resumen --app "$LIVE_PID" --fin orden --linea-base 0 --modo estabilidad --entradas "0.1,0.1,0.5,0.1,0.1" -- sleep)
echo "$OUT3"
echo "$OUT3" | grep -q "solicitudes=5"
echo "$OUT3" | grep -q "latencia_alta"

echo ""
echo "=== [4/4] Modo fallo controlado sobre app viva (conserva PID vivo) ==="
OUT4=$(TRACELITE_DIR="$TMP_DIR/c4" "$BIN" --resumen --app "$LIVE_PID" --fin orden --linea-base 0 --modo fallo --fallo-en 3 --entradas "0.1,0.1,0.1,0.1,0.1" -- sleep)
echo "$OUT4"
echo "$OUT4" | grep -q "error_proceso"
kill -0 "$LIVE_PID"

echo ""
echo "[OK] pruebas_vivos.sh completado con exito"
