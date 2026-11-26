#!/usr/bin/env bash
set -euo pipefail

# Pruebas integrales con programas y servicios vivos reales del SO
RAIZ="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="$RAIZ/bin/tracelite"

TMP_DIR="$(mktemp -d /tmp/tl_vivos_XXXX)"
BE_PID=""
LIVE_PID=""
limpiar() {
    [ -n "$BE_PID" ] && kill "$BE_PID" 2>/dev/null || true
    [ -n "$LIVE_PID" ] && kill "$LIVE_PID" 2>/dev/null || true
    rm -rf "$TMP_DIR"
}
trap limpiar EXIT
export XDG_RUNTIME_DIR="$TMP_DIR"

echo "=== [1/4] Observación de proceso vivo con subprocesos intermitentes (--resumen) ==="
python3 -c 'import subprocess, time; [ (subprocess.Popen(["sleep", "0.4"]).wait(), time.sleep(0.25)) for _ in range(3) ]' &
LIVE_PID=$!
OUT1=$(TRACELITE_DIR="$TMP_DIR/c1" "$BIN" --resumen -d 2 --interval 100 "$LIVE_PID")
echo "$OUT1"
echo "$OUT1" | grep -q "RELACION"
echo "$OUT1" | grep -q "orquestador ->"
wait "$LIVE_PID" 2>/dev/null || true
LIVE_PID=""

echo ""
echo "=== [2/4] Backend HTTP vivo con anomalía intermedia (estabilidad: 4x32MB + 384MB) ==="
python3 "$RAIZ/docs/pruebas/backend_demo.py" 18091 &
BE_PID=$!
sleep 0.3
OUT2=$(TRACELITE_DIR="$TMP_DIR/c2" "$BIN" --resumen --fuente proc --linea-base 0 --modo estabilidad --backend 127.0.0.1:18091 --ruta /hash --entradas "mb=32,mb=32,mb=384,mb=32,mb=32")
echo "$OUT2"
echo "$OUT2" | grep -q "cliente_http"
echo "$OUT2" | grep -q "sensor_so"
echo "$OUT2" | grep -q "alertas=1"
kill -0 "$BE_PID"
kill "$BE_PID" 2>/dev/null || true
wait "$BE_PID" 2>/dev/null || true
BE_PID=""

echo ""
echo "=== [3/4] Escalado lineal contra backend vivo (mb=32,64,128,256,512) ==="
python3 "$RAIZ/docs/pruebas/backend_demo.py" 18092 &
BE_PID=$!
sleep 0.3
OUT3=$(TRACELITE_DIR="$TMP_DIR/c3" "$BIN" --resumen --fuente proc --linea-base 0 --modo escalado --backend 127.0.0.1:18092 --ruta /hash --entradas "mb=32,mb=64,mb=128,mb=256,mb=512" --tamanos "1,2,4,8,16")
echo "$OUT3"
echo "$OUT3" | grep -q "solicitudes=5"
kill "$BE_PID" 2>/dev/null || true
wait "$BE_PID" 2>/dev/null || true
BE_PID=""

echo ""
echo "=== [4/4] Programa con latencia controlada (sleep) y SLO excedido ==="
OUT4=$(TRACELITE_DIR="$TMP_DIR/c4" "$BIN" --resumen --modo respuesta --slo-ms 20 --entradas "0.15,0.15" -- sleep)
echo "$OUT4"
echo "$OUT4" | grep -q "lanzador ->"
echo "$OUT4" | grep -q "slo_excedido"

echo ""
echo "[OK] pruebas_vivos.sh completado con exito"
