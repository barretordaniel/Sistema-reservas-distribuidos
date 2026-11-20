#!/usr/bin/env bash
# Demostración rápida en UNA máquina de la arquitectura escalable y tolerante a fallas:
# persistencia (4 hilos), cancelaciones, GT principal + GT de relevo, 2 "VMs" de RMC (vm4 y vm5,
# 2 hilos cada una) y el generador de carga repartido en 2 "VMs" (--vm-indice/--vm-total).
# Los números de esta prueba NO sirven para el informe: todo comparte la misma máquina.
#
# Uso: scripts/prueba_local.sh [duracion_s=20] [clientes=8] [solo_consulta=4] [extra args del generador...]
#      DB="dbname=srb" scripts/prueba_local.sh 30 8 4 --rpm 0
set -u
cd "$(dirname "$0")/.."
DUR=${1:-20}
CLI=${2:-8}
SOLO=${3:-4}
shift 3 2>/dev/null || shift $# 2>/dev/null
DB=${DB:-dbname=srb}

mkdir -p logs
PIDS=()
cleanup() { kill -TERM "${PIDS[@]}" 2>/dev/null; sleep 0.5; kill -KILL "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

./bin/persistencia --db "$DB" --reset --hilos 4 > logs/persistencia.log 2>&1 &
PERS=$!; PIDS+=($PERS)
sleep 1.5
./bin/cancelaciones > logs/cancelaciones.log 2>&1 &                                       PIDS+=($!)
./bin/gestor --nombre GT1 --stats-csv logs/gt1.csv > logs/gt1.log 2>&1 &                  PIDS+=($!); GT1=$!
./bin/gestor --nombre GT2 --bind tcp://*:6555 --bind-rmc tcp://*:6556 \
             > logs/gt2.log 2>&1 &                                                         PIDS+=($!)
GTS=tcp://localhost:5556,tcp://localhost:6556
./bin/rmc --nombre vm4 --hilos 2 --gt $GTS > logs/rmc_vm4.log 2>&1 &                      PIDS+=($!)
./bin/rmc --nombre vm5 --hilos 2 --gt $GTS > logs/rmc_vm5.log 2>&1 &                      PIDS+=($!)
sleep 2

GENS=()
for i in 0 1; do
    ./bin/generador --clientes "$CLI" --solo-consulta "$SOLO" --duracion "$DUR" --calentamiento 2 \
        --vm-indice $i --vm-total 2 --gt tcp://localhost:5555,tcp://localhost:6555 \
        --arquitectura escalable --csv logs/resultados.csv --muestras logs/muestras_vm$i.csv "$@" \
        > logs/generador_vm$i.log 2>&1 &
    GENS+=($!)
done
wait "${GENS[@]}"
for i in 0 1; do echo "--- generador VM $i ---"; grep CARGA logs/generador_vm$i.log | tail -n 6; done

kill -USR1 "$PERS" "$GT1"; sleep 0.7
echo; echo "== Estado final de la BD (ultimas lineas) =="; tail -n 3 logs/persistencia.log
echo "== Resumen del GT principal =="; grep GT-STATS logs/gt1.log | tail -n 6
