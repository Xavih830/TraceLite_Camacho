#!/usr/bin/env bash
set -euo pipefail

RAIZ="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$RAIZ/bin/tracelite"

TMP_RUN="$(mktemp -d /tmp/tl_sol_XXXX)"
trap 'rm -rf "$TMP_RUN"' EXIT
export XDG_RUNTIME_DIR="$TMP_RUN"
export TRACELITE_DIR="$TMP_RUN"
export COLUMNS=220

echo "test_solicitudes" > "$TMP_RUN/datos.bin"

echo "[1/7] Instantánea -- sha256sum (1 traza, 2 spans)..."
OUT1=$("$BIN" -- sha256sum "$TMP_RUN/datos.bin")
echo "$OUT1" | grep -q "solicitudes=1 spans=2 alertas=0 errores=0"
echo "$OUT1" | grep -q "servicio_a"
echo "$OUT1" | grep -q "servicio_b"
echo "  OK"

echo "[2/7] Resumen -n 5 -- sha256sum (5 trazas, 10 spans)..."
OUT2=$("$BIN" --resumen -n 5 -- sha256sum "$TMP_RUN/datos.bin")
echo "$OUT2" | grep -q "solicitudes=5 spans=10 alertas=0 errores=0"
echo "  OK"

echo "[3/7] ps --forest muestra servicio_a y servicio_b como hijos de tracelite..."
"$BIN" --resumen -n 1 -- sleep 0.6 > /dev/null &
TPID=$!
sleep 0.2
FOREST=$(ps -eo pid,ppid,comm --forest | grep -E "[t]racelite|servicio_a|servicio_b" || true)
wait "$TPID"
echo "$FOREST" | grep -q "servicio_a"
echo "$FOREST" | grep -q "servicio_b"
echo "  OK"

echo "[4/7] Comando inexistente -> ERROR..."
set +e
OUT4=$("$BIN" -- comando_que_no_existe_xyz 2>&1)
set -e
echo "$OUT4" | grep -q "ERROR"
echo "$OUT4" | grep -q "errores=2"
echo "  OK"

echo "[5/7] Plazo vencido (--plazo 1 -- sleep 5)..."
OUT5=$("$BIN" --plazo 1 -- sleep 5)
echo "$OUT5" | grep -q "ERROR"
grep -q "plazo_vencido" "$TMP_RUN"/*.jsonl
echo "  OK"

echo "[6/7] kill -9 a servicio_b no bloquea..."
"$BIN" --resumen -n 1 -- sleep 0.5 > "$TMP_RUN/killb.txt" 2>&1 &
TPID=$!
sleep 0.12
pkill -9 -P "$TPID" -x servicio_b 2>/dev/null || true
wait "$TPID" || true
grep -q "errores=1" "$TMP_RUN/killb.txt"
grep -q "servicio_b_caido" "$TMP_RUN"/*.jsonl
echo "  OK"

echo "[7/7] Combinaciones inválidas (código 2) y limpieza final..."
set +e
"$BIN" -n 5 -- sleep 0.1 2>/dev/null; RC1=$?
"$BIN" --lote /dev/null -- sleep 0.1 2>/dev/null; RC2=$?
"$BIN" --resumen -d 2 -- sleep 0.1 2>/dev/null; RC3=$?
set -e
[ "$RC1" -eq 2 ] && [ "$RC2" -eq 2 ] && [ "$RC3" -eq 2 ]
[ -z "$(find "$TMP_RUN" -maxdepth 1 -name 'tracelite-*')" ]
! pgrep -x "servicio_a" >/dev/null 2>&1
! pgrep -x "servicio_b" >/dev/null 2>&1
echo "  OK"

echo "[OK] test_solicitudes.sh superado con exito"
