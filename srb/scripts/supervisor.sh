#!/usr/bin/env bash
# Supervisor: mantiene vivo un servicio. Si el proceso muere de forma inesperada (kill -9, fallo,
# error de memoria...), arranca un proceso NUEVO que lo sustituye; el servicio se vuelve a registrar
# por su cuenta (el RMC envía READY al GT; el GT, la persistencia y las cancelaciones reabren sus
# sockets) y la operación que estaba en curso la reasigna el GT a otra réplica.
#
# Uso:   scripts/supervisor.sh bin/rmc --nombre vm4 --hilos 2 --gt tcp://IP_GT:5556
# Para detenerlo: Ctrl-C o kill <pid del supervisor>  (detiene también al servicio).
# Si el servicio termina con código 0 (parada ordenada) el supervisor no lo relanza.
[ $# -ge 1 ] || { echo "Uso: $0 <comando> [argumentos...]"; exit 2; }
parar=0
hijo=0
trap 'parar=1; [ "$hijo" -gt 0 ] && kill -TERM "$hijo" 2>/dev/null' TERM INT
reinicios=0
while [ "$parar" = 0 ]; do
    "$@" &
    hijo=$!
    wait "$hijo"
    codigo=$?
    [ "$parar" = 1 ] && break
    [ "$codigo" = 0 ] && break
    reinicios=$((reinicios + 1))
    echo "[SUPERVISOR $(date +%H:%M:%S)] $1 terminó con código $codigo; arranco un proceso nuevo (reinicio #$reinicios)"
    sleep 1
done
wait 2>/dev/null
