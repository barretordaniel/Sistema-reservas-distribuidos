#!/usr/bin/env bash
# Pruebas automáticas del ESCALADO POR CPU (scale-out / scale-in) y del LEAKY BUCKET, todo en una máquina.
# La CPU de cada "VM" se simula con un archivo (--cpu-simulada) para que la prueba sea determinista: se sube y
# se baja el valor a mano y se comprueba qué hace el GT. Lo ejecuta también scripts/pruebas_automaticas.sh.
#
# Uso: scripts/pruebas_escalado.sh
set -u
cd "$(dirname "$0")/.."
DB=${DB:-dbname=srb}
LOG=logs/pruebas_escalado
mkdir -p "$LOG"; rm -f "$LOG"/*
PIDS=()
PASS=0; FAIL=0
ok()   { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
check() { if [ "$1" = "1" ]; then ok "$2"; else bad "$2"; fi; }
sql()  { psql -d "${DB#dbname=}" -P pager=off -tA -c "$1" 2>/dev/null; }
lanzar() { local n=$1; shift; "$@" > "$LOG/$n.log" 2>&1 & ULTIMO=$!; PIDS+=($ULTIMO); }
parar_todo() {
    for p in "${PIDS[@]:-}"; do kill -TERM "$p" 2>/dev/null; done
    sleep 1
    for p in "${PIDS[@]:-}"; do kill -KILL "$p" 2>/dev/null; done
    wait 2>/dev/null; PIDS=()
}
trap parar_todo EXIT
campo() { grep "errores=" "$1" | tail -1 | sed -n "s/.*[ ]$2=\([0-9]*\).*/\1/p"; }
cpu() { echo "$2" > "$LOG/cpu_$1"; }   # cpu <vm> <valor>: fija la CPU simulada de esa VM

# suma de una columna del CSV de línea de tiempo en las filas con niveles_activos == $2 que repiten el nivel de
# la fila anterior (se descarta la fila del cambio, donde el reparto es mixto).  suma <csv> <nivel> <columna>
suma() {
    awk -F, -v n="$2" -v c="$3" 'NR==1{for(i=1;i<=NF;i++) if($i==c) k=i; next}
        {if ($3==n && $3==prev) s+=$k; prev=$3} END{print s+0}' "$1"
}
filas() {  # filas <csv> <nivel>: cuántas filas estables hay con ese número de niveles activos
    awk -F, -v n="$2" 'NR>1{if ($3==n && $3==prev) s++; prev=$3} END{print s+0}' "$1"
}

arrancar_base() {  # arrancar_base <prueba> <args extra del GT>
    local t=$1; shift
    lanzar "$t.persistencia" bin/persistencia --reset --silencioso --db "$DB" --hilos 4
    sleep 1.5
    lanzar "$t.cancelaciones" bin/cancelaciones --silencioso
    lanzar "$t.gt1" bin/gestor --silencioso --nombre GT1 --linea-tiempo "$LOG/$t.csv" "$@"
}

echo "== 10. Scale-out / scale-in por CPU (métrica 'ultimo'): base -> vm4 -> vm5 y de vuelta (CPU simulada) =="
cpu base 10; cpu vm4 5; cpu vm5 5
arrancar_base esc --escalar base,vm4,vm5 --umbral-out 50 --umbral-in 20 --sostenido-out 1 --sostenido-in 3 --enfriar 2 --escala-metrica ultimo
for v in base vm4 vm5; do
    lanzar "esc.rmc_$v" bin/rmc --silencioso --nombre $v --hilos 2 --gt tcp://localhost:5556 --cpu-simulada "$LOG/cpu_$v"
done
sleep 2.5
timeout 90 bin/generador --clientes 8 --solo-consulta 2 --pct-consulta 20 --pct-reserva 50 --pct-modificar 30 \
    --rpm 0 --duracion 44 --calentamiento 0 --gt tcp://localhost:5555 --timeout-ms 4000 > "$LOG/esc.gen.log" 2>&1 &
GEN=$!
sleep 7;  echo "    t=7s   base al 60 %  (la primera VM supera el 50 %)";            cpu base 60
sleep 8;  echo "    t=15s  vm4 al 60 %   (la segunda también)";                        cpu vm4 60
sleep 8;  echo "    t=23s  la carga baja: base 30 %, vm4 5 %, vm5 5 %";                cpu base 30; cpu vm4 5; cpu vm5 5
wait $GEN
grep -E "ESCALA|activ" "$LOG/esc.gt1.log" | sed 's/^/    /'
outs=$(grep -c "ESCALA-OUT" "$LOG/esc.gt1.log"); ins=$(grep -c "ESCALA-IN" "$LOG/esc.gt1.log")
check "$(grep -q "ESCALA-OUT.*'base'.*se activa 'vm4'" "$LOG/esc.gt1.log" && echo 1 || echo 0)" "con base >= 50 % se activa vm4 (scale-out 1)"
check "$(grep -q "ESCALA-OUT.*'vm4'.*se activa 'vm5'" "$LOG/esc.gt1.log" && echo 1 || echo 0)" "con vm4 >= 50 % se activa vm5 (scale-out 2)"
check "$([ "$outs" = "2" ] && echo 1 || echo 0)" "exactamente 2 scale-out, sin oscilaciones [$outs]"
check "$(grep -q "ESCALA-IN.*'vm5'" "$LOG/esc.gt1.log" && grep -q "ESCALA-IN.*'vm4'" "$LOG/esc.gt1.log" && echo 1 || echo 0)" "al bajar la carga se liberan vm5 y luego vm4 (scale-in)"
check "$([ "$ins" = "2" ] && echo 1 || echo 0)" "exactamente 2 scale-in [$ins]"
csv="$LOG/esc.csv"
d1b=$(suma $csv 1 desp_base); d1o=$(( $(suma $csv 1 desp_vm4) + $(suma $csv 1 desp_vm5) ))
check "$([ "$d1b" -gt 0 ] && [ "$d1o" = "0" ] && echo 1 || echo 0)" "con 1 nivel activo todo el trabajo va a base [base=$d1b vm4+vm5=$d1o en $(filas $csv 1) s]"
d2b=$(suma $csv 2 desp_base); d24=$(suma $csv 2 desp_vm4); d25=$(suma $csv 2 desp_vm5)
check "$([ "$d2b" -gt 0 ] && [ "$d24" -gt 0 ] && [ "$d25" = "0" ] && echo 1 || echo 0)" "con 2 niveles se reparte entre base y vm4, vm5 sigue sin trabajo [base=$d2b vm4=$d24 vm5=$d25]"
d3b=$(suma $csv 3 desp_base); d34=$(suma $csv 3 desp_vm4); d35=$(suma $csv 3 desp_vm5)
check "$([ "$d3b" -gt 0 ] && [ "$d34" -gt 0 ] && [ "$d35" -gt 0 ] && echo 1 || echo 0)" "con 3 niveles trabajan las tres VMs [base=$d3b vm4=$d34 vm5=$d35]"
if [ "$d34" -gt 0 ] && [ "$d35" -gt 0 ]; then
    r=$(awk -v a="$d3b" -v b="$d34" -v c="$d35" 'BEGIN{m=a;if(b<m)m=b;if(c<m)m=c;M=a;if(b>M)M=b;if(c>M)M=c;print (M-m)/M*100 <= 15 ? 1 : 0}')
    check "$r" "con 3 niveles el reparto es parejo (diferencia máxima <= 15 %)"
fi
ult=$(tail -1 "$csv" | cut -d, -f3)
check "$([ "$ult" = "1" ] && echo 1 || echo 0)" "al final solo queda 1 nivel activo [$ult]"
err=$(campo "$LOG/esc.gen.log" errores); sin=$(campo "$LOG/esc.gen.log" reservas_sin_respuesta)
conf=$(campo "$LOG/esc.gen.log" reservas_confirmadas); filas_bd=$(sql "select count(*) from reserva")
check "$([ "$err" = "0" ] && [ "$sin" = "0" ] && echo 1 || echo 0)" "los cambios de nivel son transparentes: errores=$err, sin respuesta=$sin"
check "$([ "$conf" = "$filas_bd" ] && echo 1 || echo 0)" "reservas confirmadas = filas de la BD [$conf = $filas_bd]"
parar_todo

echo "== 11. Leaky bucket: se activa cuando todas las VMs están al límite y limita la entrada =="
cpu base 10
arrancar_base cub --escalar base --umbral-out 50 --sostenido-out 1 --cubeta 10 --fuga 100
lanzar cub.rmc_base bin/rmc --silencioso --nombre base --hilos 4 --gt tcp://localhost:5556 --cpu-simulada "$LOG/cpu_base"
sleep 2.5
timeout 90 bin/generador --clientes 30 --solo-consulta 6 --pct-consulta 20 --pct-reserva 60 --pct-modificar 20 \
    --rpm 0 --duracion 26 --calentamiento 0 --gt tcp://localhost:5555 --timeout-ms 4000 > "$LOG/cub.gen.log" 2>&1 &
GEN=$!
sleep 8;  echo "    t=8s   la única VM disponible pasa al 80 %  (no hay más niveles que activar)";  cpu base 80
sleep 10; echo "    t=18s  la carga cede: base 10 %";                                                  cpu base 10
wait $GEN
grep -E "CUBETA" "$LOG/cub.gt1.log" | sed 's/^/    /'
csv="$LOG/cub.csv"
check "$(grep -q "CUBETA ACTIVA" "$LOG/cub.gt1.log" && echo 1 || echo 0)" "con el último nivel al límite entra el leaky bucket"
check "$(grep -q "CUBETA LIBERADA" "$LOG/cub.gt1.log" && echo 1 || echo 0)" "al ceder la carga se libera la cubeta"
# filas con cubeta activa estables / sin cubeta antes de activarse
read -r tpm_on rech_on cola_max n_on <<<"$(awk -F, 'NR>1{if($4==1 && pc==1){r+=$6; s+=$7; if($5>m)m=$5; n++} pc=$4} END{printf "%.0f %d %d %d", (n?r/n:0), s, m, n}' "$csv")"
read -r tpm_off rech_off n_off <<<"$(awk -F, 'NR>1{if($4==0 && pc==0 && !hecho){r+=$6; s+=$7; n++} if($4==1) hecho=1; pc=$4} END{printf "%.0f %d %d", (n?r/n:0), s, n}' "$csv")"
echo "    sin cubeta: ~$tpm_off respondidas/s (rechazadas $rech_off en $n_off s)   con cubeta: ~$tpm_on respondidas/s (rechazadas $rech_on en $n_on s, cola máx $cola_max)"
check "$([ "$rech_off" = "0" ] && echo 1 || echo 0)" "antes de activarse la cubeta no se rechaza nada [$rech_off]"
check "$([ "$tpm_on" -ge 70 ] && [ "$tpm_on" -le 130 ] && echo 1 || echo 0)" "con la cubeta activa la salida queda en ~100 solicitudes/s (fuga) [$tpm_on/s]"
check "$([ "$tpm_off" -gt $((tpm_on * 3)) ] && echo 1 || echo 0)" "sin cubeta el sistema procesa mucho más que con ella (el tope es deliberado) [$tpm_off vs $tpm_on]"
check "$([ "$rech_on" -gt 0 ] && echo 1 || echo 0)" "con la cubeta activa el exceso se rechaza de inmediato (SOBRECARGA) [$rech_on]"
check "$([ "$cola_max" -le 11 ] && echo 1 || echo 0)" "la cola nunca supera la capacidad de la cubeta (10) [máx $cola_max]"
sob=$(campo "$LOG/cub.gen.log" sobrecarga)
check "$([ "${sob:-0}" -gt 0 ] && echo 1 || echo 0)" "los clientes recibieron los rechazos por SOBRECARGA [$sob]"
tail_rows=$(tail -3 "$csv" | awk -F, '{r+=$6; s+=$7} END{printf "%d %d", r/3, s}')
check "$([ "${tail_rows% *}" -gt 300 ] && [ "${tail_rows#* }" = "0" ] && echo 1 || echo 0)" "tras liberarse la cubeta el sistema vuelve a todo su ritmo sin rechazos [${tail_rows% *}/s, rechazos ${tail_rows#* }]"
err=$(campo "$LOG/cub.gen.log" errores); sin=$(campo "$LOG/cub.gen.log" reservas_sin_respuesta)
conf=$(campo "$LOG/cub.gen.log" reservas_confirmadas); filas_bd=$(sql "select count(*) from reserva")
check "$([ "$err" = "0" ] && [ "$sin" = "0" ] && echo 1 || echo 0)" "rechazar por sobrecarga no genera errores ni solicitudes sin respuesta (errores=$err, sin respuesta=$sin)"
check "$([ "$conf" = "$filas_bd" ] && echo 1 || echo 0)" "lo rechazado no dejó rastro en la BD: confirmadas = filas [$conf = $filas_bd]"
neg=$(sql "select count(*) from evento where asientos_libres<0 or asientos_ocupados<0")
inc=$(sql "select count(*) from evento e where e.asientos_ocupados <> coalesce((select sum(num_asientos) from reserva r where r.id_evento=e.id_evento and r.id_ocurrencia=e.id_ocurrencia and r.activa),0)")
check "$([ "$neg" = "0" ] && [ "$inc" = "0" ] && echo 1 || echo 0)" "invariantes de la BD intactas (sin asientos negativos, ocupados = reservas activas)"
parar_todo

echo "== 12. Métrica 'max' (por defecto): el GT calienta la VM base y vm4/vm5 solo hacen de RMC (caso real) =="
# En las VMs reales el GT vive en la primera VM: esa VM sube del 50 % mientras vm4 y vm5 apenas pasan del 10 %.
# Con la regla "la VM más cargada de las activas" la cascada igual debe completarse y terminar en el leaky bucket.
cpu base 15; cpu vm4 8; cpu vm5 8
arrancar_base max --escalar base,vm4,vm5 --umbral-out 50 --umbral-in 20 --sostenido-out 1 --sostenido-in 3 --enfriar 2 --cubeta 50
for v in base vm4 vm5; do
    lanzar "max.rmc_$v" bin/rmc --silencioso --nombre $v --hilos 2 --gt tcp://localhost:5556 --cpu-simulada "$LOG/cpu_$v"
done
sleep 2.5
timeout 90 bin/generador --clientes 8 --solo-consulta 2 --pct-consulta 20 --pct-reserva 50 --pct-modificar 30 \
    --rpm 0 --duracion 36 --calentamiento 0 --gt tcp://localhost:5555 --timeout-ms 4000 > "$LOG/max.gen.log" 2>&1 &
GEN=$!
sleep 6;  echo "    t=6s   base al 65 % (el GT la calienta), vm4 y vm5 al 10 %";  cpu base 65; cpu vm4 10; cpu vm5 10
sleep 16; echo "    t=22s  la carga baja: base 30 %, vm4 y vm5 5 %";             cpu base 30; cpu vm4 5; cpu vm5 5
wait $GEN
grep -E "ESCALA|CUBETA" "$LOG/max.gt1.log" | sed 's/^/    /'
l1=$(grep -n "ESCALA-OUT.*'base'.*se activa 'vm4'" "$LOG/max.gt1.log" | head -1 | cut -d: -f1)
l2=$(grep -n "ESCALA-OUT.*'base'.*se activa 'vm5'" "$LOG/max.gt1.log" | head -1 | cut -d: -f1)
l3=$(grep -n "CUBETA ACTIVA" "$LOG/max.gt1.log" | head -1 | cut -d: -f1)
outs=$(grep -c "ESCALA-OUT" "$LOG/max.gt1.log"); ins=$(grep -c "ESCALA-IN" "$LOG/max.gt1.log")
check "$([ -n "$l1" ] && [ -n "$l2" ] && [ -n "$l3" ] && [ "$l1" -lt "$l2" ] && [ "$l2" -lt "$l3" ] && echo 1 || echo 0)" "cascada completa en orden: base -> vm4 -> vm5 -> leaky bucket aunque vm4/vm5 estén casi libres [$l1 < $l2 < $l3]"
check "$([ "$outs" = "2" ] && echo 1 || echo 0)" "exactamente 2 scale-out [$outs]"
csv="$LOG/max.csv"
# mientras la base sigue caliente (>= 40 %) no se libera ninguna VM, aunque vm4 y vm5 estén por debajo del 20 %
cal=$(awk -F, 'NR>1 && $7+0>=0 {if($3==3 && $4==1) n++} END{print n+0}' "$csv")
check "$([ "$cal" -ge 5 ] && echo 1 || echo 0)" "con la base caliente se mantienen 3 niveles y la cubeta activa [$cal s]"
check "$(grep -q "CUBETA LIBERADA" "$LOG/max.gt1.log" && echo 1 || echo 0)" "al enfriarse la base se libera la cubeta"
check "$(grep -q "ESCALA-IN.*'vm5'" "$LOG/max.gt1.log" && grep -q "ESCALA-IN.*'vm4'" "$LOG/max.gt1.log" && [ "$ins" = "2" ] && echo 1 || echo 0)" "al enfriarse la base se liberan vm5 y luego vm4 (scale-in) [$ins]"
# la liberación ocurre DESPUÉS de que la base baja a 30 % (t=22 s), no antes
primer_in=$(awk -F, 'NR>1{if(pn==3 && $3<3){print NR; exit} pn=$3}' "$csv")
check "$([ -n "$primer_in" ] && [ "$primer_in" -ge 18 ] && echo 1 || echo 0)" "no se libera nada mientras la base está caliente (primer scale-in en la fila $primer_in del CSV)"
d3b=$(suma $csv 3 desp_base); d34=$(suma $csv 3 desp_vm4); d35=$(suma $csv 3 desp_vm5)
check "$([ "$d3b" -gt 0 ] && [ "$d34" -gt 0 ] && [ "$d35" -gt 0 ] && echo 1 || echo 0)" "con 3 niveles trabajan las tres VMs [base=$d3b vm4=$d34 vm5=$d35]"
err=$(campo "$LOG/max.gen.log" errores); sin=$(campo "$LOG/max.gen.log" reservas_sin_respuesta)
conf=$(campo "$LOG/max.gen.log" reservas_confirmadas); filas_bd=$(sql "select count(*) from reserva")
check "$([ "$err" = "0" ] && [ "$sin" = "0" ] && [ "$conf" = "$filas_bd" ] && echo 1 || echo 0)" "sin errores ni pérdidas: errores=$err, sin respuesta=$sin, confirmadas=$conf = filas $filas_bd"
parar_todo

echo "== 13. Carga sintética (--carga-cpu-ms), monitores (monitor_vm.sh, monitor_bd.sh) y contador de solicitudes en vivo (--en-vivo) =="
arrancar_base car
lanzar car.rmc bin/rmc --silencioso --nombre base --hilos 1 --gt tcp://localhost:5556 --carga-cpu-ms 20
sleep 2.5
rm -f "$LOG/mon_vm.csv" "$LOG/mon_bd.csv"
scripts/monitor_vm.sh 1 --n 8 --csv "$LOG/mon_vm.csv" > "$LOG/mon_vm.txt" 2>&1 &
MON1=$!
scripts/monitor_bd.sh 1 --n 6 --csv "$LOG/mon_bd.csv" > "$LOG/mon_bd.txt" 2>&1 &
MON2=$!
timeout 60 bin/generador --clientes 4 --solo-consulta 4 --pct-consulta 100 --pct-reserva 0 --pct-modificar 0 --pct-cancelar 0 \
    --rpm 0 --duracion 8 --calentamiento 0 --gt tcp://localhost:5555 --timeout-ms 4000 --en-vivo 1 > "$LOG/car.gen.log" 2>&1
wait $MON1 $MON2 2>/dev/null
n=$(grep "CONSULTA:" "$LOG/car.gen.log" | tail -1 | sed -n 's/.*n=\([0-9]*\).*/\1/p')
check "$([ "${n:-0}" -ge 250 ] && [ "${n:-0}" -le 440 ] && echo 1 || echo 0)" "con 20 ms de CPU por solicitud un RMC de 1 hilo no pasa de ~50 solicitudes/s [${n:-0} en 8 s; sin carga serían miles]"
err=$(campo "$LOG/car.gen.log" errores)
check "$([ "$err" = "0" ] && echo 1 || echo 0)" "la carga sintética no genera errores [$err]"
filas_vm=$(awk -F, 'NR>1' "$LOG/mon_vm.csv" | wc -l)
rmc_max=$(awk -F, 'NR>1 && $5+0>m{m=$5+0} END{print m+0}' "$LOG/mon_vm.csv")
check "$([ "$filas_vm" -ge 6 ] && [ "$rmc_max" -ge 20 ] && echo 1 || echo 0)" "monitor_vm.sh: $filas_vm muestras y el RMC aparece consumiendo CPU [máx $rmc_max % de la VM]"
check "$(grep -q "GRC" "$LOG/mon_vm.txt" && echo 1 || echo 0)" "monitor_vm.sh: reconoce los procesos del SRB que corren en la VM (G=gestor R=rmc C=cancelaciones)"
filas_bd=$(awk -F, 'NR>1' "$LOG/mon_bd.csv" | wc -l)
tx_max=$(awk -F, 'NR>1 && $6+0>m{m=$6+0} END{print m+0}' "$LOG/mon_bd.csv")
check "$([ "$filas_bd" -ge 4 ] && [ "$tx_max" -gt 0 ] && echo 1 || echo 0)" "monitor_bd.sh: $filas_bd muestras y ve transacciones en la BD [máx $tx_max tx/s]"
check "$(awk -F, 'NR>1 && $7+0>=1 && $7+0<=100 {ok=1} END{print ok+0}' "$LOG/mon_bd.csv")" "monitor_bd.sh: informa la RAM usada en % (1-100)"
vivas=$(grep -c '\[EN VIVO\]' "$LOG/car.gen.log")
fin=$(grep '\[EN VIVO\] FIN' "$LOG/car.gen.log" | tail -1)
env_f=$(echo "$fin" | sed -n 's/.* enviadas=\([0-9]*\) .*/\1/p'); res_f=$(echo "$fin" | sed -n 's/.* respondidas=\([0-9]*\) .*/\1/p')
check "$([ "$vivas" -ge 8 ] && echo 1 || echo 0)" "contador en vivo (--en-vivo 1): una línea por segundo durante la corrida [$vivas líneas en 8 s]"
check "$([ -n "$env_f" ] && [ "$env_f" = "$res_f" ] && [ $((res_f - ${n:-0})) -eq 4 ] && echo 1 || echo 0)" "el contador en vivo cuadra con el informe final: enviadas=$env_f respondidas=$res_f = CONSULTA n=${n:-0} + 4 consultas iniciales"
check "$(! grep '\[EN VIVO\]' "$LOG/car.gen.log" | grep -qE 'CONSULTA:|RESERVA:|MODIFIC|errores=' && echo 1 || echo 0)" "las líneas en vivo no se confunden con el resumen que leen los scripts (no contienen 'CONSULTA:' ni 'errores=')"
parar_todo

echo
echo "RESUMEN: $PASS pruebas correctas, $FAIL fallidas   (logs en $LOG/)"
[ "$FAIL" = "0" ]
