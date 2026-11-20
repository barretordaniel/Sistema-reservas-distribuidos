#!/usr/bin/env bash
# Batería de pruebas automáticas, TODO EN UNA MÁQUINA (los procesos simulan las VMs con puertos
# y nombres distintos). Comprueba funcionalidad, consistencia bajo concurrencia, reparto
# equitativo entre VMs, caída de un RMC, caída del GT principal, generador repartido en 2 VMs y, al final,
# el escalado por CPU y el leaky bucket (scripts/pruebas_escalado.sh).
#
# Uso: scripts/pruebas_automaticas.sh [segundos_por_prueba_de_carga]   (por defecto 15)
# Requiere: make ya ejecutado, PostgreSQL con la base "srb" accesible sin contraseña.
set -u
cd "$(dirname "$0")/.."
DUR=${1:-15}
DB=${DB:-dbname=srb}
LOG=logs/pruebas
mkdir -p "$LOG"; rm -f "$LOG"/*
PIDS=()
PASS=0; FAIL=0

ok()   { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
check() { if [ "$1" = "1" ]; then ok "$2"; else bad "$2"; fi; }
sql()  { psql -d "${DB#dbname=}" -P pager=off -tA -c "$1" 2>/dev/null; }

lanzar() {  # lanzar <nombre> <comando...>   (deja el pid en $ULTIMO)
    local nombre=$1; shift
    "$@" > "$LOG/$nombre.log" 2>&1 &
    ULTIMO=$!; PIDS+=($ULTIMO)
}
parar_todo() {
    for p in "${PIDS[@]:-}"; do kill -TERM "$p" 2>/dev/null; done
    sleep 1
    for p in "${PIDS[@]:-}"; do kill -KILL "$p" 2>/dev/null; done
    wait 2>/dev/null
    PIDS=()
}
trap parar_todo EXIT

# arrancar_sistema <nombre_prueba> <args de persistencia> <hilos_rmc_por_vm> [con_gt_relevo]
arrancar_sistema() {
    local t=$1 pers=$2 hilos=$3 relevo=${4:-no}
    lanzar "$t.persistencia" bin/persistencia --reset --silencioso --db "$DB" $pers; PERS=$ULTIMO
    sleep 1.5
    lanzar "$t.cancelaciones" bin/cancelaciones --silencioso
    local gts="tcp://localhost:5556"
    lanzar "$t.gt1" bin/gestor --silencioso --nombre GT1 --stats-csv "$LOG/$t.gt1.csv"; GT1=$ULTIMO
    if [ "$relevo" = "si" ]; then
        gts="tcp://localhost:5556,tcp://localhost:6556"
        lanzar "$t.gt2" bin/gestor --silencioso --nombre GT2 --bind tcp://*:6555 --bind-rmc tcp://*:6556 \
               --stats-csv "$LOG/$t.gt2.csv"; GT2=$ULTIMO
    fi
    lanzar "$t.rmc_vm4" bin/rmc --silencioso --nombre vm4 --hilos "$hilos" --gt "$gts"; RMC4=$ULTIMO
    lanzar "$t.rmc_vm5" bin/rmc --silencioso --nombre vm5 --hilos "$hilos" --gt "$gts"; RMC5=$ULTIMO
    sleep 2
}

campo() {  # campo <archivo> <clave>  -> valor de clave=valor en la última línea "errores=..."
    grep "errores=" "$1" | tail -1 | sed -n "s/.*[ ]$2=\([0-9]*\).*/\1/p"
}

# Invariantes de la BD: se cumplen siempre, pase lo que pase durante la carga.
invariantes() {  # invariantes <total_asientos_esperado>
    local esperado=$1
    local neg; neg=$(sql "select count(*) from evento where asientos_libres<0 or asientos_ocupados<0")
    check "$([ "$neg" = "0" ] && echo 1 || echo 0)" "ninguna ocurrencia con asientos negativos (sin sobreventa)"
    local tot; tot=$(sql "select sum(asientos_libres+asientos_ocupados) from evento")
    check "$([ "$tot" = "$esperado" ] && echo 1 || echo 0)" "libres + ocupados = $esperado (no se perdieron ni crearon asientos) [$tot]"
    local inc; inc=$(sql "select count(*) from evento e where e.asientos_ocupados <> coalesce((select sum(num_asientos) from reserva r where r.id_evento=e.id_evento and r.id_ocurrencia=e.id_ocurrencia and r.activa),0)")
    check "$([ "$inc" = "0" ] && echo 1 || echo 0)" "asientos ocupados = suma de las reservas activas en todas las ocurrencias [incoherentes=$inc]"
}

echo "== 1. Pruebas unitarias del mensaje =="
if bin/test_mensaje > "$LOG/unitarias.log" 2>&1; then ok "$(tail -1 "$LOG/unitarias.log")"; else bad "unitarias (ver $LOG/unitarias.log)"; fi

echo "== 2. Prueba funcional (reservas, modificaciones, cancelaciones, idempotencia, mensajes inválidos) =="
arrancar_sistema funcional "--hilos 4" 2
if bin/prueba_funcional > "$LOG/funcional.log" 2>&1; then ok "$(tail -1 "$LOG/funcional.log")"; else bad "funcional: $(grep FAIL "$LOG/funcional.log" | head -3 | tr '\n' ' ')"; fi
a=$(sql "select 'ocupados='||sum(asientos_ocupados)||' libres='||sum(asientos_libres) from evento")
b=$(sql "select 'reservas_activas='||count(*) filter (where activa)||' reservas_total='||count(*) from reserva")
esp=$(grep ESPERADO_BD "$LOG/funcional.log" | sed 's/ESPERADO_BD //')
check "$([ "$a $b" = "$esp" ] && echo 1 || echo 0)" "estado final de la BD = esperado [$a $b]"
invariantes 7000
parar_todo

echo "== 3. Concurrencia: 8 clientes compitiendo por 120 asientos (sin pausa, ${DUR}s) =="
arrancar_sistema concurrencia "--hilos 4 --eventos 3 --dobles 1 --asientos 30" 2
timeout $((DUR+40)) bin/generador --clientes 8 --solo-consulta 2 --rpm 0 --duracion "$DUR" --calentamiento 0 \
    --eventos 3 --dobles 1 --max-asientos 5 --pct-consulta 20 --pct-reserva 40 --pct-modificar 30 --pct-cancelar 10 \
    --gt tcp://localhost:5555 --notif tcp://localhost:5558 > "$LOG/concurrencia.gen.log" 2>&1
tail -4 "$LOG/concurrencia.gen.log" | sed 's/^/    /'
invariantes 120
err=$(campo "$LOG/concurrencia.gen.log" errores); sin=$(campo "$LOG/concurrencia.gen.log" reservas_sin_respuesta)
conf=$(campo "$LOG/concurrencia.gen.log" reservas_confirmadas); filas=$(sql "select count(*) from reserva")
check "$([ "$err" = "0" ] && echo 1 || echo 0)" "ninguna respuesta de error [errores=$err]"
check "$([ "$sin" = "0" ] && echo 1 || echo 0)" "ninguna solicitud sin respuesta [$sin]"
check "$([ "$conf" = "$filas" ] && echo 1 || echo 0)" "reservas confirmadas a los clientes = filas en la tabla reserva [$conf = $filas]"
parar_todo

echo "== 4. Reparto equitativo entre las 2 VMs de RMC (carga normal, ${DUR}s) =="
arrancar_sistema reparto "--hilos 4" 2
timeout $((DUR+40)) bin/generador --clientes 8 --solo-consulta 4 --rpm 600 --duracion "$DUR" --calentamiento 2 \
    --gt tcp://localhost:5555 --arquitectura escalable --csv "$LOG/reparto.csv" > "$LOG/reparto.gen.log" 2>&1
kill -USR1 $GT1; sleep 0.7
tail -5 "$LOG/reparto.gen.log" | sed 's/^/    /'
rep=$(grep "reparto entre VMs" "$LOG/reparto.gt1.log" | tail -1); echo "    $rep"
v4=$(echo "$rep" | sed -n 's/.*vm4=\([0-9]*\).*/\1/p'); v5=$(echo "$rep" | sed -n 's/.*vm5=\([0-9]*\).*/\1/p')
if [ -n "$v4" ] && [ -n "$v5" ] && [ $((v4+v5)) -gt 0 ]; then
    dif=$(( v4>v5 ? v4-v5 : v5-v4 )); tot=$((v4+v5))
    check "$([ $((dif*100/tot)) -le 5 ] && echo 1 || echo 0)" "reparto equitativo: vm4=$v4 vm5=$v5 (diferencia $((dif*100/tot))% <= 5%)"
else bad "no se encontró el reparto en el GT"; fi
err=$(campo "$LOG/reparto.gen.log" errores); check "$([ "$err" = "0" ] && echo 1 || echo 0)" "sin errores [$err]"
invariantes 7000
parar_todo

echo "== 5. Falla de una VM de RMC en plena carga, con asientos escasos (se apaga vm4 con kill -9 a los 6 s) =="
# Asientos escasos (120 en total): la solicitud reasignada vuelve a ejecutarse cuando casi no hay cupo, que es el
# caso difícil: un RECHAZO por "falta de cupo" no puede esconder una reserva que el primer intento sí guardó.
arrancar_sistema falla_rmc "--hilos 4 --eventos 3 --dobles 1 --asientos 30" 2
( sleep 6; kill -KILL $RMC4 ) &
timeout $((DUR+40)) bin/generador --clientes 8 --solo-consulta 4 --rpm 0 --duracion "$DUR" --calentamiento 0 \
    --eventos 3 --dobles 1 --max-asientos 5 --pct-consulta 20 --pct-reserva 50 --pct-modificar 30 \
    --gt tcp://localhost:5555 --timeout-ms 4000 > "$LOG/falla_rmc.gen.log" 2>&1
kill -USR1 $GT1; sleep 0.7
tail -4 "$LOG/falla_rmc.gen.log" | sed 's/^/    /'
grep -E "se da por caído|reasigna" "$LOG/falla_rmc.gt1.log" | head -3 | sed 's/^/    /'
caidos=$(grep -c "se da por caído" "$LOG/falla_rmc.gt1.log")
check "$([ "$caidos" -ge 2 ] && echo 1 || echo 0)" "el GT detectó la caída de las 2 réplicas de vm4 por falta de heartbeat [$caidos]"
err=$(campo "$LOG/falla_rmc.gen.log" errores); sin=$(campo "$LOG/falla_rmc.gen.log" reservas_sin_respuesta)
conf=$(campo "$LOG/falla_rmc.gen.log" reservas_confirmadas); filas=$(sql "select count(*) from reserva")
check "$([ "$err" = "0" ] && echo 1 || echo 0)" "la falla fue transparente: ningún error para los clientes [errores=$err]"
check "$([ "$sin" = "0" ] && echo 1 || echo 0)" "ninguna reserva quedó sin respuesta [$sin]"
check "$([ "$conf" = "$filas" ] && echo 1 || echo 0)" "ninguna reserva perdida ni duplicada: confirmadas = filas [$conf = $filas]"
invariantes 120
parar_todo

echo "== 6. Falla del GT principal en plena carga (kill -9 a los 6 s; los clientes pasan al GT de relevo) =="
arrancar_sistema falla_gt "--hilos 4" 2 si
( sleep 6; kill -KILL $GT1 ) &
timeout $((DUR+60)) bin/generador --clientes 8 --solo-consulta 4 --rpm 0 --duracion $((DUR+8)) --calentamiento 0 \
    --gt tcp://localhost:5555,tcp://localhost:6555 --timeout-ms 2000 --reintentos 3 > "$LOG/falla_gt.gen.log" 2>&1
tail -4 "$LOG/falla_gt.gen.log" | sed 's/^/    /'
cambios=$(campo "$LOG/falla_gt.gen.log" cambios_de_GT); err=$(campo "$LOG/falla_gt.gen.log" errores)
sin=$(campo "$LOG/falla_gt.gen.log" reservas_sin_respuesta); conf=$(campo "$LOG/falla_gt.gen.log" reservas_confirmadas)
filas=$(sql "select count(*) from reserva")
check "$([ "${cambios:-0}" -ge 1 ] && echo 1 || echo 0)" "los clientes pasaron al GT de relevo [cambios_de_GT=$cambios]"
check "$([ "$err" = "0" ] && echo 1 || echo 0)" "ningún error para los clientes [errores=$err]"
check "$([ "$sin" = "0" ] && echo 1 || echo 0)" "ninguna reserva quedó sin respuesta [$sin]"
check "$([ "$conf" = "$filas" ] && echo 1 || echo 0)" "los reintentos no duplicaron ni perdieron reservas: confirmadas = filas [$conf = $filas]"
invariantes 7000
parar_todo

echo "== 7. Generador repartido en 2 VMs (mismo comando, --vm-indice 0 y 1) =="
arrancar_sistema generador2 "--hilos 4" 2
GENS=()
for i in 0 1; do
    timeout $((DUR+40)) bin/generador --clientes 8 --solo-consulta 4 --vm-indice $i --vm-total 2 --rpm 300 \
        --duracion "$DUR" --calentamiento 2 --gt tcp://localhost:5555 --arquitectura escalable --repeticion 1 \
        --muestras "$LOG/muestras_vm$i.csv" --csv "$LOG/generador2_vm$i.csv" > "$LOG/generador2.vm$i.log" 2>&1 &
    GENS+=($!)
done
wait "${GENS[@]}"
for i in 0 1; do head -1 "$LOG/generador2.vm$i.log" | sed 's/^/    /'; done
c0=$(head -1 "$LOG/generador2.vm0.log" | sed -n 's/.*: \([0-9]*\) de 8 clientes.*/\1/p')
c1=$(head -1 "$LOG/generador2.vm1.log" | sed -n 's/.*: \([0-9]*\) de 8 clientes.*/\1/p')
check "$([ "$c0" = "4" ] && [ "$c1" = "4" ] && echo 1 || echo 0)" "8 clientes repartidos 4 + 4 entre las 2 VMs [$c0 + $c1]"
ids=$(sql "select count(distinct id_cliente) from reserva")
check "$([ "${ids:-0}" = "4" ] && echo 1 || echo 0)" "los 4 clientes que reservan (2 por VM) tienen ids distintos entre VMs [$ids]"
vmm=$(cat "$LOG"/muestras_vm0.csv "$LOG"/muestras_vm1.csv | grep -v '^arquitectura' | cut -d, -f4 | sort -u | tr '\n' ' ')
check "$([ "$vmm" = "0 1 " ] && echo 1 || echo 0)" "cada VM generadora escribe su propio CSV de muestras (vm_indice $vmm)"
invariantes 7000
parar_todo

echo "== 8. Cliente de archivo: caso de ejemplo del profesor (BD antes y después) =="
arrancar_sistema cliente_archivo "--hilos 4" 2
antes=$(sql "select 'ocupados='||sum(asientos_ocupados)||' libres='||sum(asientos_libres) from evento")
check "$([ "$antes" = "ocupados=0 libres=7000" ] && echo 1 || echo 0)" "BD inicial: 50 eventos, 20 con 2 ocurrencias, 100 asientos cada una [$antes]"
bin/cliente ejemplos/caso_ejemplo.txt --esperar-notif 2000 > "$LOG/cliente_archivo.log" 2>&1
despues=$(sql "select 'ocupados='||sum(asientos_ocupados)||' libres='||sum(asientos_libres) from evento")
check "$([ "$despues" = "ocupados=12 libres=6988" ] && echo 1 || echo 0)" "BD final = la esperada para el caso [$despues]"
act=$(sql "select string_agg(id_reserva||':E'||id_evento||'/o'||id_ocurrencia||'x'||num_asientos, ' ' order by id_reserva) from reserva where activa")
check "$([ "$act" = "1:E1/o1x3 2:E2/o2x5 3:E30/o1x4" ] && echo 1 || echo 0)" "reservas activas = 1:E1/o1x3 2:E2/o2x5 3:E30/o1x4 [$act]"
check "$(grep -q 'NOTIFICACION cliente=1: cancelación de la reserva 4' "$LOG/cliente_archivo.log" && echo 1 || echo 0)" "la cancelación asíncrona terminó con su notificación al cliente"
check "$(grep -q 'MODIFICAR_FECHA 3 .* -> RECHAZO' "$LOG/cliente_archivo.log" && echo 1 || echo 0)" "cambiar fecha de un evento de una sola ocurrencia se rechaza"
invariantes 7000
parar_todo

echo "== 9. Supervisor: un proceso nuevo sustituye al RMC que cae (kill -9 a los 5 s) =="
lanzar sup.persistencia bin/persistencia --reset --silencioso --hilos 4; sleep 1.5
lanzar sup.cancelaciones bin/cancelaciones --silencioso
lanzar sup.gt1 bin/gestor --silencioso --nombre GT1; GT1=$ULTIMO
lanzar sup.rmc_vm4 scripts/supervisor.sh bin/rmc --silencioso --nombre vm4 --hilos 2; SUP=$ULTIMO
lanzar sup.rmc_vm5 bin/rmc --silencioso --nombre vm5 --hilos 2
sleep 2
( sleep 5; kill -KILL $(pgrep -P $SUP -x rmc) 2>/dev/null ) &
timeout $((DUR+40)) bin/generador --clientes 8 --solo-consulta 4 --rpm 0 --duracion "$DUR" --calentamiento 0 \
    --gt tcp://localhost:5555 --timeout-ms 4000 > "$LOG/supervisor.gen.log" 2>&1
tail -3 "$LOG/supervisor.gen.log" | sed 's/^/    /'
grep -E "SUPERVISOR" "$LOG/sup.rmc_vm4.log" | head -2 | sed 's/^/    /'
check "$(grep -q 'arranco un proceso nuevo' "$LOG/sup.rmc_vm4.log" && echo 1 || echo 0)" "el supervisor arrancó un proceso nuevo tras la caída"
reg=$(grep -c "RMC registrado: vm4" "$LOG/sup.gt1.log")
check "$([ "$reg" -ge 4 ] && echo 1 || echo 0)" "el GT registró de nuevo las réplicas de vm4 (2 al inicio + 2 del proceso nuevo) [$reg]"
err=$(campo "$LOG/supervisor.gen.log" errores); sin=$(campo "$LOG/supervisor.gen.log" reservas_sin_respuesta)
conf=$(campo "$LOG/supervisor.gen.log" reservas_confirmadas); filas=$(sql "select count(*) from reserva")
check "$([ "$err" = "0" ] && [ "$sin" = "0" ] && echo 1 || echo 0)" "falla transparente: errores=$err, sin respuesta=$sin"
check "$([ "$conf" = "$filas" ] && echo 1 || echo 0)" "confirmadas = filas de la BD [$conf = $filas]"
invariantes 7000
kill -TERM $SUP 2>/dev/null
parar_todo

echo "== 10-11. Escalado por CPU (scale-out/scale-in) y leaky bucket (scripts/pruebas_escalado.sh) =="
scripts/pruebas_escalado.sh > "$LOG/escalado.log" 2>&1
sed -n '/^== 10/,$p' "$LOG/escalado.log" | grep -v "^RESUMEN" | grep -v "^$" | sed 's/^== 1[01]\..*/&/'
res=$(grep "^RESUMEN" "$LOG/escalado.log" | tail -1)
pe=$(echo "$res" | sed -n 's/^RESUMEN: \([0-9]*\) pruebas.*/\1/p'); fe=$(echo "$res" | sed -n 's/.* \([0-9]*\) fallidas.*/\1/p')
PASS=$((PASS + ${pe:-0})); FAIL=$((FAIL + ${fe:-1}))

echo
echo "RESUMEN: $PASS pruebas correctas, $FAIL fallidas   (logs en $LOG/)"
[ "$FAIL" = "0" ]
