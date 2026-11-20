#!/usr/bin/env bash
# Prueba empírica del SRB, DESDE EL NODO GENERADOR DE CARGA (VM1).
# Arranca la topología en las otras VMs por ssh, genera la carga desde esta máquina, mide la CPU de cada
# componente en cada nodo mientras corre, y al final dice dónde está el cuello de botella.
#
#   scripts/prueba_empirica.sh mapa                      quién es quién (qué rol cumple cada VM)
#   scripts/prueba_empirica.sh verificar                 ssh, binarios compilados, puertos
#   scripts/prueba_empirica.sh correr base               1 RMC de 1 hilo + persistencia de 1 hilo (3 VMs)
#   scripts/prueba_empirica.sh correr escalable          RMC en VM4+VM5, persistencia multihilo, GT de relevo
#   scripts/prueba_empirica.sh correr elastica           arranca como "base" y a los ELASTICA_S s une VM4 y VM5
#                                                        EN PLENA CARGA (se ve el salto de throughput)
#   scripts/prueba_empirica.sh escalamiento             SCALE-OUT / SCALE-IN por CPU con rampa de carga: VM3 sirve sola
#                                                        hasta el 50 % de CPU, luego se activa VM4, luego VM5; al
#                                                        saturarse todas entra el leaky bucket; al bajar la carga se liberan
#   scripts/prueba_empirica.sh test1                     UNA VM alcanza: carga ligera; comprueba que no hay scale-out ni cubeta
#   scripts/prueba_empirica.sh test2                     rampa PESADA hasta ocupar las 3 VMs (scale-out a vm4 y vm5 + leaky
#                                                        bucket); usa CPU sintética en el RMC (CARGA_T ms por solicitud)
#   scripts/prueba_empirica.sh procedimientos base|escalable
#                                                        aísla cada procedimiento (consulta, reserva, modificación,
#                                                        cancelación, mixto) y dice cuánta CPU gasta cada componente
#   scripts/prueba_empirica.sh parar                     detiene los procesos del SRB en todas las VMs
#
# Configuración: copie scripts/vms.conf.ejemplo a scripts/vms.conf y ponga las IP reales.
# Requisitos: ssh sin contraseña desde esta VM hacia VM2, VM3, VM4 y VM5 (ver README), el proyecto compilado
# (make) en la MISMA ruta en todas las VMs, y PostgreSQL con la base "srb" en la VM de la BD (IP_BD).
#
# Variables opcionales: CLIENTES="3 5 8" REPS=1 DUR=60 CAL=10 HILOS_BD=4 HILOS_RMC=2 ASIENTOS=5000
#                       ELASTICA_S=25 SALIDA=resultados_empiricos MODO=local (ensayo en una sola máquina)
#                       CLIENTES_PROC=8 (clientes en 'procedimientos')  SALIDA_PROC=resultados_procedimientos
#                       Escalamiento: PASOS="4 12 24 48 96 160 96 48 12 4" PASO_S=25 UMBRAL_OUT=50 UMBRAL_IN=20 CUBETA=60
#                       FUGA=0 (req/s de salida de la cubeta; 0 = solo limita la cola) HILOS_ESC=4 ASIENTOS_ESC=200000
#                       REPOSO_S=40 (segundos sin carga al final: se ve el scale-in)  METRICA=max|ultimo (CPU que decide el scale-out)
#                       RPM_ESC=0 (solicitudes/min por cliente; 0 = sin pausa)  CARGA_MS=0 (CPU sintética por solicitud en el RMC)
# Contador EN VIVO (solicitudes enviadas/respondidas/por segundo/en curso/rechazadas/fallas) en esta terminal mientras
#   corre cada paso: EN_VIVO=1 (segundos entre líneas; 0 = apagado). Sale por stderr: con "| tee archivo" el archivo no
#   lo guarda (use "2>&1 | tee" si lo quiere); la línea completa de cada paso queda siempre en el .gen.log de resultados.
# test1 y test2 (scripts/test1.sh, scripts/test2.sh): ver más abajo y README. test1 = una VM alcanza (~1 min); test2 = rampa hasta
#   ocupar las 3 VMs (~3 min). Variables: T1_PASOS T1_RPM T1_PASO_S  T2_PASOS T2_PASO_S T2_REPOSO_S CUBETA_T2  CARGA_T (ms de CPU por solicitud)  HILOS_T
#   Son 3 veces más rápidas que antes (pasos de 10 s en vez de 30 s; escalado con T_SOSTENIDO_IN=5 y T_ENFRIAR_S=5 en vez de 10 y 8).
#   Para la versión lenta de antes:  T1_PASO_S=30 T2_PASO_S=30 T2_REPOSO_S=45 T1_REPOSO_S=15 T_SOSTENIDO_IN=10 T_ENFRIAR_S=8 scripts/test2.sh
# Etiquetas de las VMs en los informes (vms.conf): ET_GT ET_BD ET_VM4 ET_VM5 ET_GEN, p. ej. ET_BD="VM2-BD".
# Si la capa lógica está toda en UNA VM, ponga IP_VM4 e IP_VM5 iguales a IP_GT (y ET_VM4="" ET_VM5=""):
# 'escalable' arranca entonces el GT de relevo y las réplicas RMC en esa misma VM.
set -u
cd "$(dirname "$0")/.."
CONF=${CONF:-scripts/vms.conf}
# shellcheck disable=SC1090
[ -f "$CONF" ] && . "$CONF"
MODO=${MODO:-ssh}
USUARIO=${USUARIO:-$(whoami)}
RUTA=${RUTA:-$PWD}
IP_GT=${IP_GT:-localhost}; IP_BD=${IP_BD:-localhost}; IP_VM4=${IP_VM4:-localhost}; IP_VM5=${IP_VM5:-localhost}
SSH_OPTS=${SSH_OPTS:--o BatchMode=yes -o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new}
CLIENTES=${CLIENTES:-"3 5 8"}; REPS=${REPS:-1}; DUR=${DUR:-60}; CAL=${CAL:-10}
HILOS_BD=${HILOS_BD:-4}; HILOS_RMC=${HILOS_RMC:-2}; ASIENTOS=${ASIENTOS:-5000}
ELASTICA_S=${ELASTICA_S:-25}
CLIENTES_PROC=${CLIENTES_PROC:-8}
PASOS=${PASOS:-"4 12 24 48 96 160 96 48 12 4"}; PASO_S=${PASO_S:-25}
UMBRAL_OUT=${UMBRAL_OUT:-50}; UMBRAL_IN=${UMBRAL_IN:-20}; CUBETA=${CUBETA:-60}; FUGA=${FUGA:-0}
REPOSO_S=${REPOSO_S:-40}; METRICA=${METRICA:-max}
RPM_ESC=${RPM_ESC:-0}        # solicitudes por minuto de cada cliente en 'escalamiento' (0 = sin pausa, máxima carga)
CARGA_MS=${CARGA_MS:-0}      # CPU sintética por solicitud en cada RMC (ms); 0 = ninguna. Ver --carga-cpu-ms de bin/rmc
TITULO_ESC=${TITULO_ESC:-}; VEREDICTO_ESC=${VEREDICTO_ESC:-}
SOSTENIDO_OUT=${SOSTENIDO_OUT:-3}; SOSTENIDO_IN=${SOSTENIDO_IN:-10}; ENFRIAR_S=${ENFRIAR_S:-8}   # tiempos del escalado (s)
EN_VIVO=${EN_VIVO:-1}        # segundos entre líneas del contador en vivo del generador (0 = apagado)
HILOS_ESC=${HILOS_ESC:-4}; ASIENTOS_ESC=${ASIENTOS_ESC:-200000}
ET_GT=${ET_GT-VM3(GT)}; ET_BD=${ET_BD-VM2(BD)}; ET_VM4=${ET_VM4-VM4}; ET_VM5=${ET_VM5-VM5}; ET_GEN=${ET_GEN-VM1(generador)}
SALIDA=${SALIDA:-resultados_empiricos}
HOSTS=$(printf '%s\n' "$IP_GT" "$IP_BD" "$IP_VM4" "$IP_VM5" | awk '!v[$0]++')

# Usuario y carpeta del proyecto de cada VM: por defecto USUARIO y RUTA (iguales en todas); si las VMs son cuentas de
# personas distintas, ponga en vms.conf USUARIO_GT/RUTA_GT, USUARIO_BD/RUTA_BD, USUARIO_VM4/RUTA_VM4, USUARIO_VM5/RUTA_VM5.
usuario_de() {
    local u=$USUARIO
    [ "$1" = "$IP_BD" ] && u=${USUARIO_BD:-$u}; [ "$1" = "$IP_GT" ] && u=${USUARIO_GT:-$u}
    [ "$1" = "$IP_VM4" ] && u=${USUARIO_VM4:-$u}; [ "$1" = "$IP_VM5" ] && u=${USUARIO_VM5:-$u}
    echo "$u"
}
ruta_de() {
    local r=$RUTA
    [ "$1" = "$IP_BD" ] && r=${RUTA_BD:-$r}; [ "$1" = "$IP_GT" ] && r=${RUTA_GT:-$r}
    [ "$1" = "$IP_VM4" ] && r=${RUTA_VM4:-$r}; [ "$1" = "$IP_VM5" ] && r=${RUTA_VM5:-$r}
    echo "$r"
}
en() {  # en <ip> <comando>: ejecuta en esa VM, dentro de la carpeta del proyecto
    local ip=$1; shift
    if [ "$MODO" = "local" ]; then bash -c "cd '$RUTA' && $*"
    else ssh $SSH_OPTS "$(usuario_de "$ip")@$ip" "cd '$(ruta_de "$ip")' && $*"; fi
}
lanzar() {  # lanzar <ip> <nombre_log> <comando>: arranca en segundo plano, sobrevive al cierre de ssh
    en "$1" "mkdir -p logs && (setsid nohup $3 > logs/$2.log 2>&1 < /dev/null &); sleep 0.2; true"
}
gen_vivo() {  # gen_vivo <log> <timeout_s> <args del generador...>: corre bin/generador; el detalle va al log y,
              # si EN_VIVO>0, el contador "[EN VIVO]" de cada segundo se ve en esta terminal (stderr)
    local log=$1 lim=$2; shift 2
    if [ "${EN_VIVO:-0}" -gt 0 ] 2>/dev/null; then
        timeout "$lim" bin/generador "$@" --en-vivo "$EN_VIVO" 2>&1 | tee "$log" |
            grep --line-buffered '\[EN VIVO\]' | sed -u 's/^\[\([0-9:]*\)\.[0-9]*\] \[EN VIVO\]/   \1 EN VIVO/' >&2
    else
        timeout "$lim" bin/generador "$@" > "$log" 2>&1
    fi
}
rol_de() {  # etiqueta (sin espacios) de los roles que tiene una IP según vms.conf
    local ip=$1 r=""
    [ "$ip" = "$IP_GT" ] && [ -n "$ET_GT" ] && r="$r+$ET_GT"; [ "$ip" = "$IP_BD" ] && [ -n "$ET_BD" ] && r="$r+$ET_BD"
    [ "$ip" = "$IP_VM4" ] && [ -n "$ET_VM4" ] && r="$r+$ET_VM4"; [ "$ip" = "$IP_VM5" ] && [ -n "$ET_VM5" ] && r="$r+$ET_VM5"
    r=${r#+}; echo "${r:-$ip}" | tr ' ' '_'
}

cmd_mapa() {
    echo "Según scripts/vms.conf:   GT+cancelaciones = $IP_GT ($(rol_de "$IP_GT"))   BD = $IP_BD ($(rol_de "$IP_BD"))   VM4 = $IP_VM4   VM5 = $IP_VM5"
    echo "Lo que dice cada máquina:"
    for ip in $HOSTS; do
        printf '  %-16s ' "$ip"; en "$ip" "scripts/quien_soy.sh --corto" 2>&1 | tail -1
    done
    echo "(Esta máquina es el generador, $ET_GEN: $(hostname) [$(hostname -I 2>/dev/null | awk '{print $1}')])"
}

cmd_verificar() {
    local mal=0
    for ip in $HOSTS; do
        echo "-- $ip  ($(rol_de "$ip"))"
        if ! en "$ip" "hostname" >/dev/null 2>&1; then
            echo "   ERROR: no hay ssh sin contraseña hacia $(usuario_de "$ip")@$ip, o no existe la carpeta $(ruta_de "$ip") (ver 'Preparar el ssh' en el README)"; mal=1; continue
        fi
        faltan=""
        necesarios=""   # solo los binarios que esa VM necesita según su rol
        [ "$ip" = "$IP_BD" ] && necesarios="$necesarios persistencia"
        [ "$ip" = "$IP_GT" ] && necesarios="$necesarios gestor cancelaciones rmc"
        [ "$ip" = "$IP_VM4" ] && necesarios="$necesarios gestor rmc"
        [ "$ip" = "$IP_VM5" ] && necesarios="$necesarios rmc"
        for b in $(echo $necesarios | tr ' ' '\n' | sort -u); do
            en "$ip" "test -x bin/$b" 2>/dev/null || faltan="$faltan $b"
        done
        [ -n "$faltan" ] && { echo "   AVISO: faltan binarios:$faltan (en esa VM: cd $(ruta_de "$ip") && make$(for f in $faltan; do printf ' bin/%s' $f; done) -- ver 'Preparar VM4 y VM5' en el README)"; }
        en "$ip" "test -x scripts/muestreo_cpu.sh" 2>/dev/null || { echo "   ERROR: falta scripts/muestreo_cpu.sh (copie la carpeta scripts/)"; mal=1; }
        echo "   ok: $(en "$ip" 'hostname; nproc' | paste -sd' ') núcleos"
    done
    test -x bin/generador || { echo "ERROR: compile el generador en esta máquina (make)"; mal=1; }
    echo "Puertos que esta VM necesita alcanzar (si el SRB ya está corriendo): GT $IP_GT:5555 y :5558"
    for p in 5555 5558; do
        timeout 3 bash -c "</dev/tcp/$IP_GT/$p" 2>/dev/null && echo "   $IP_GT:$p  abierto" || echo "   $IP_GT:$p  cerrado (¿GT/cancelaciones sin arrancar o firewall?)"
    done
    return $mal
}

parar_todo() {
    for ip in $HOSTS; do
        en "$ip" "pkill -TERM -f '[s]upervisor.sh'; pkill -TERM -f '[b]in/(persistencia|gestor|rmc|cancelaciones)'; sleep 0.7; pkill -KILL -f '[b]in/(persistencia|gestor|rmc|cancelaciones)'; true" 2>/dev/null
    done
}

lanzar_rmcs_vm4_vm5() {
    local gts="tcp://$IP_GT:5556,tcp://$IP_VM4:6556"
    lanzar "$IP_VM4" rmc_vm4 "bin/rmc --silencioso --nombre vm4 --hilos $HILOS_RMC --gt $gts --persistencia tcp://$IP_BD:5559"
    lanzar "$IP_VM5" rmc_vm5 "bin/rmc --silencioso --nombre vm5 --hilos $HILOS_RMC --gt $gts --persistencia tcp://$IP_BD:5559"
}

esperar_registro() {  # esperar_registro <cuántos RMC (hilos) debe ver el GT>
    local n=0
    for _ in $(seq 1 25); do
        n=$(en "$IP_GT" "grep -c 'RMC registrado' logs/gt1.log 2>/dev/null; true" | tail -1)
        [ "${n:-0}" -ge "$1" ] && { echo "   el GT ve $n trabajadores RMC registrados"; return 0; }
        sleep 1
    done
    echo "   ERROR: el GT solo ve ${n:-0} de $1 trabajadores RMC. Revise (logs/ en cada VM):"
    echo "     - que el RMC de VM4/VM5 compiló y arrancó (logs/rmc_vm4.log, logs/rmc_vm5.log)"
    echo "     - que VM4/VM5 alcanzan $IP_GT:5556 y $IP_BD:5559 (firewall: 'nc -zv $IP_GT 5556')"
    return 1
}

arrancar() {  # arrancar <base|escalable|elastica>
    local arq=$1 hb=$HILOS_BD
    [ "$arq" = base ] && hb=1
    ASIENTOS_USO=$ASIENTOS; [ "$arq" = escalamiento ] && ASIENTOS_USO=$ASIENTOS_ESC
    echo ">> Arrancando topología '$arq' (persistencia con $hb hilos)"
    parar_todo; sleep 1
    lanzar "$IP_BD" persistencia "bin/persistencia --reset --silencioso --hilos $hb --asientos $ASIENTOS_USO"
    sleep 2.5
    lanzar "$IP_GT" cancelaciones "bin/cancelaciones --silencioso --persistencia tcp://$IP_BD:5559"
    [ "$arq" != escalamiento ] && lanzar "$IP_GT" gt1 "bin/gestor --silencioso --nombre GT1 --cancel tcp://localhost:5557 --stats-csv logs/gt1.csv"
    case $arq in
        escalamiento)   # niveles en orden de activación: base (VM3) -> vm4 -> vm5; el GT decide según la CPU de cada VM
            en "$IP_GT" "rm -f logs/escala.csv"
            lanzar "$IP_GT" gt1 "bin/gestor --silencioso --nombre GT1 --cancel tcp://localhost:5557 --escalar base,vm4,vm5 --umbral-out $UMBRAL_OUT --umbral-in $UMBRAL_IN --sostenido-out $SOSTENIDO_OUT --sostenido-in $SOSTENIDO_IN --enfriar $ENFRIAR_S --escala-metrica $METRICA --cubeta $CUBETA $( [ "$FUGA" != 0 ] && echo "--fuga $FUGA" ) --linea-tiempo logs/escala.csv --stats-csv logs/gt1.csv"
            local carga="" prio=""
            if [ "$CARGA_MS" != 0 ]; then carga="--carga-cpu-ms $CARGA_MS"; prio="nice -n 10 "; fi   # el GT pasa antes que el RMC de su VM
            lanzar "$IP_GT" rmc_base "${prio}bin/rmc --silencioso --nombre base --hilos $HILOS_ESC $carga --gt tcp://localhost:5556 --persistencia tcp://$IP_BD:5559"
            lanzar "$IP_VM4" rmc_vm4 "bin/rmc --silencioso --nombre vm4 --hilos $HILOS_ESC $carga --gt tcp://$IP_GT:5556 --persistencia tcp://$IP_BD:5559"
            lanzar "$IP_VM5" rmc_vm5 "bin/rmc --silencioso --nombre vm5 --hilos $HILOS_ESC $carga --gt tcp://$IP_GT:5556 --persistencia tcp://$IP_BD:5559"
            ESPERADOS=$((3 * HILOS_ESC)) ;;
        base)
            lanzar "$IP_GT" rmc_base "bin/rmc --silencioso --nombre base --hilos 1 --gt tcp://localhost:5556 --persistencia tcp://$IP_BD:5559"
            ESPERADOS=1 ;;
        escalable)
            lanzar "$IP_VM4" gt2 "bin/gestor --silencioso --nombre GT2 --bind tcp://*:6555 --bind-rmc tcp://*:6556 --cancel tcp://$IP_GT:5557"
            sleep 1; lanzar_rmcs_vm4_vm5
            ESPERADOS=$((2 * HILOS_RMC)) ;;
        elastica)
            lanzar "$IP_VM4" gt2 "bin/gestor --silencioso --nombre GT2 --bind tcp://*:6555 --bind-rmc tcp://*:6556 --cancel tcp://$IP_GT:5557"
            sleep 1
            lanzar "$IP_GT" rmc_base "bin/rmc --silencioso --nombre base --hilos 1 --gt tcp://$IP_GT:5556,tcp://$IP_VM4:6556 --persistencia tcp://$IP_BD:5559"
            ESPERADOS=1 ;;
    esac
    esperar_registro "$ESPERADOS"
}

perfil() {  # perfil del enunciado para N clientes (o el perfil fijado por 'procedimientos')
    [ -n "${PERFIL_EXTRA:-}" ] && { echo "$PERFIL_EXTRA"; return; }
    case $1 in
        3) echo "--solo-consulta 2 --pct-consulta 50 --pct-reserva 50 --pct-modificar 0" ;;
        5) echo "--solo-consulta 3 --pct-consulta 0 --pct-reserva 70 --pct-modificar 30" ;;
        *) echo "--solo-consulta $(( $1 / 2 )) --pct-consulta 0 --pct-reserva 70 --pct-modificar 30" ;;
    esac
}

un_escenario() {  # un_escenario <arq> <clientes> <repeticion> [etiqueta_de_procedimiento]
    local arq=$1 N=$2 R=$3 W=$((DUR - 3)) gts="tcp://$IP_GT:5555" earq=$1 etq
    [ -n "${4:-}" ] && earq="$1-$4"
    etq="${earq}_c${N}_r${R}"
    [ "$arq" != base ] && gts="$gts,tcp://$IP_VM4:6555"
    mkdir -p "$SALIDA/tmp"; rm -f "$SALIDA"/tmp/*
    echo; echo "== corrida $etq: $N clientes, ${CAL}s de calentamiento + ${DUR}s medidos${PERFIL_EXTRA:+  [perfil: $PERFIL_EXTRA]} =="
    en "$IP_BD" "pkill -USR2 -f '[b]in/persistencia'"; sleep 4          # BD al estado inicial
    en "$IP_GT" "pkill -USR1 -f '[b]in/gestor .*GT1'; true"; sleep 1     # contadores del GT en cero

    SAMPLERS=()
    for ip in $HOSTS; do   # muestreo de CPU en cada nodo, alineado con la ventana medida
        en "$ip" "scripts/muestreo_cpu.sh $CAL $W '$(rol_de "$ip")'" > "$SALIDA/tmp/cpu_$ip.txt" 2>&1 &
        SAMPLERS+=($!)
    done
    if [ "$MODO" != local ]; then   # la CPU de ESTA máquina (generador): si se satura, el cuello sería la carga y no el SRB
        scripts/muestreo_cpu.sh "$CAL" "$W" "$(echo "$ET_GEN" | tr ' ' '_')" > "$SALIDA/tmp/cpu_generador.txt" 2>&1 &
        SAMPLERS+=($!)
    fi
    if [ "$arq" = elastica ]; then   # VM4 y VM5 se unen en plena carga
        ( sleep $((CAL + ELASTICA_S)); echo "   >>> $(date +%H:%M:%S) se unen RMC de VM4 y VM5 <<<"; lanzar_rmcs_vm4_vm5 ) &
        SAMPLERS+=($!)
    fi
    # shellcheck disable=SC2046
    gen_vivo "$SALIDA/${etq}.gen.log" $((CAL + DUR + 40)) --clientes "$N" $(perfil "$N") --rpm 0 \
        --duracion $((CAL + DUR)) --calentamiento "$CAL" --gt "$gts" --notif "tcp://$IP_GT:5558" \
        --arquitectura "$earq" --repeticion "$R" --muestras "$SALIDA/${etq}_muestras.csv"
    wait "${SAMPLERS[@]}" 2>/dev/null

    # --- rendimiento visto por los clientes
    grep -E "CONSULTA:|RESERVA:|MODIFIC|errores=" "$SALIDA/${etq}.gen.log" | sed 's/^\[[0-9:.]*\] \[CARGA\] /   /'
    local n; n=$(grep -E "CONSULTA:|RESERVA:|MODIFIC" "$SALIDA/${etq}.gen.log" | sed -n 's/.* n=\([0-9]*\) .*/\1/p' | paste -sd+ | bc)
    n=${n:-0}
    # --- reparto de trabajo entre VMs según el GT
    en "$IP_GT" "pkill -USR1 -f '[b]in/gestor .*GT1'; sleep 0.8; grep 'reparto entre VMs' logs/gt1.log | tail -1" 2>/dev/null | sed 's/^\[[0-9:.]*\] /   /'
    # --- CPU por nodo y componente
    echo "   CPU durante la ventana medida (${W}s):"
    printf '   %-22s %-14s %8s %14s\n' "Nodo" "Componente" "CPU (s)" "% de 1 núcleo"
    cat "$SALIDA"/tmp/cpu_*.txt | awk '$1=="MUESTREO" && ($4>0.05){printf "   %-22s %-14s %8.2f %13s%%\n",$2,$3,$4,$5}'
    cat "$SALIDA"/tmp/cpu_*.txt | awk '$1=="NODO"{printf "   >> nodo %-22s CPU total %3s%% de %d núcleos (iowait %s%%)\n",$2,$3,$5,$4}'
    # --- ms de CPU por transacción y cuello de botella
    cat "$SALIDA"/tmp/cpu_*.txt | awk -v n="$n" -v dur="$DUR" -v w="$W" '
        $1=="MUESTREO" && $3!="generador" {cpu[$3]+=$4; tot+=$4}
        END{ if(n>0){ printf "   CPU por transacción (todas las VMs):"; for(k in cpu) printf "  %s=%.3f ms", k, 1000*cpu[k]*dur/(w*n); printf "  TOTAL=%.3f ms\n", 1000*tot*dur/(w*n)}
             m=0; for(k in cpu) if(cpu[k]>m){m=cpu[k]; mk=k}
             if(tot>0) printf "   Componente que más CPU consume: %s (%.0f%% de la CPU del sistema)\n", mk, 100*m/tot }'
    cat "$SALIDA"/tmp/cpu_*.txt | awk '$1=="NODO" && $3 ~ /^[0-9]+$/ && $3>=85{printf "   *** nodo %s SATURADO (CPU total %d%%) ***\n",$2,$3}'
    echo "$earq,$N,$R,$n" >> "$SALIDA/resumen.csv"
}

cmd_correr() {
    local arq=${1:-}
    case $arq in base|escalable|elastica) ;; *) echo "Uso: $0 correr base|escalable|elastica"; exit 2 ;; esac
    test -x bin/generador || { echo "Falta bin/generador: haga 'make' en esta máquina"; exit 1; }
    mkdir -p "$SALIDA"
    arrancar "$arq" || { echo "No se pudo completar el arranque: corrijo y vuelvo a intentar (use '$0 parar' antes)"; exit 1; }
    for N in $CLIENTES; do
        for R in $(seq 1 "$REPS"); do un_escenario "$arq" "$N" "$R"; done
    done
    if [ "$arq" = elastica ]; then
        echo; echo "== Throughput por segundo (ops/s) cada 5 s; las réplicas se unen en t=$((CAL + ELASTICA_S)) s =="
        for f in "$SALIDA"/elastica_c*_r*_muestras.csv; do
            echo " $(basename "$f" _muestras.csv)"
            awk -F, 'NR>1 && $1!="arquitectura"{t=$7+0; b=int(t/5)*5; c[b]++; if(!n++||t<tmin)tmin=t; if(t>tmax)tmax=t}
                 END{for(b in c){b+=0; lo=(b>tmin)?b:tmin; hi=(b+5<tmax)?b+5:tmax; s=hi-lo; if(s<1)s=1; printf "   t=%3d s  %7.1f ops/s\n", b, c[b]/s}}' "$f" | sort
        done
    fi
    echo; echo "Listo. Archivos en $SALIDA/ . Para la Tabla 1:  python3 analisis/analisis.py '$SALIDA/*_muestras.csv' --salida $SALIDA"
    echo "Guarde también la salida de esta ejecución (la CPU por componente es evidencia para el informe)."
}

cmd_procedimientos() {  # cada procedimiento por separado, para saber qué componente limita a cada uno
    local arq=${1:-base} N=$CLIENTES_PROC
    case $arq in base|escalable) ;; *) echo "Uso: $0 procedimientos base|escalable"; exit 2 ;; esac
    test -x bin/generador || { echo "Falta bin/generador: haga 'make' en esta máquina"; exit 1; }
    SALIDA=${SALIDA_PROC:-resultados_procedimientos}; mkdir -p "$SALIDA"
    arrancar "$arq" || { echo "No se pudo completar el arranque (use '$0 parar' antes de reintentar)"; exit 1; }
    local pr
    for pr in consulta reserva modificacion cancelacion mixto; do
        case $pr in   # (los porcentajes siempre suman 100; modificar/cancelar necesitan una reserva propia previa)
            consulta)     PERFIL_EXTRA="--solo-consulta $N --pct-consulta 100 --pct-reserva 0 --pct-modificar 0 --pct-cancelar 0" ;;
            reserva)      PERFIL_EXTRA="--solo-consulta 0 --pct-consulta 0 --pct-reserva 100 --pct-modificar 0 --pct-cancelar 0" ;;
            modificacion) PERFIL_EXTRA="--solo-consulta 0 --pct-consulta 0 --pct-reserva 20 --pct-modificar 80 --pct-cancelar 0" ;;
            cancelacion)  PERFIL_EXTRA="--solo-consulta 0 --pct-consulta 0 --pct-reserva 50 --pct-modificar 0 --pct-cancelar 50" ;;
            mixto)        PERFIL_EXTRA="--solo-consulta 0 --pct-consulta 40 --pct-reserva 30 --pct-modificar 20 --pct-cancelar 10" ;;
        esac
        un_escenario "$arq" "$N" 1 "$pr"
    done
    PERFIL_EXTRA=""
    echo; echo "Listo. Archivos en $SALIDA/ . Guarde también la salida de esta ejecución."
}

cmd_escalamiento() {
    SALIDA=${SALIDA_ESC:-resultados_escalamiento}; mkdir -p "$SALIDA"; rm -f "$SALIDA"/*.csv "$SALIDA"/*.log "$SALIDA"/*.txt "$SALIDA"/*.png "$SALIDA"/*.md
    test -x bin/generador || { echo "Falta bin/generador: haga 'make' en esta máquina"; exit 1; }
    if [ "$IP_VM4" = "$IP_GT" ] || [ "$IP_VM5" = "$IP_GT" ] || [ "$IP_VM4" = "$IP_VM5" ]; then
        echo "AVISO: IP_VM4/IP_VM5 comparten VM con otro nivel. Funciona, pero el escalado no demuestra VMs distintas."
    fi
    local total=0 n
    for n in $PASOS; do total=$((total + PASO_S + 2)); done
    echo "Rampa de clientes: $PASOS  (${PASO_S}s por paso + ${REPOSO_S}s de reposo, ~$(((total + REPOSO_S) / 60)) min)  umbral-out=${UMBRAL_OUT}% umbral-in=${UMBRAL_IN}% metrica=$METRICA cubeta=$CUBETA fuga=${FUGA}/s rpm/cliente=$RPM_ESC carga_cpu=${CARGA_MS}ms hilos_rmc=$HILOS_ESC"
    arrancar escalamiento || { echo "No se pudo completar el arranque (use '$0 parar' antes de reintentar)"; exit 1; }
    en "$IP_BD" "pkill -USR2 -f '[b]in/persistencia'"; sleep 4
    # CPU por segundo de las VMs que no informan al GT (la BD y esta máquina)
    scripts/cpu_serie.sh $((total + REPOSO_S + 8)) > "$SALIDA/cpu_VM1_generador.csv" 2>&1 &
    SAMPLERS=($!)
    en "$IP_BD" "scripts/cpu_serie.sh $((total + REPOSO_S + 8))" > "$SALIDA/cpu_BD.csv" 2>&1 &
    SAMPLERS+=($!)
    echo "clientes,fila_gt,paso" > "$SALIDA/pasos.csv"
    echo "clientes,rpm_cliente,ops_s,consulta_p95_ms,reserva_p95_ms,modificacion_p95_ms,sobrecarga,errores" > "$SALIDA/pasos_resumen.csv"
    { echo "version=$(head -1 VERSION 2>/dev/null)"; echo "fecha=$(date '+%F %T')"; echo "pasos=$PASOS"; echo "paso_s=$PASO_S"
      echo "reposo_s=$REPOSO_S"; echo "rpm_por_cliente=$RPM_ESC"; echo "carga_cpu_ms_por_solicitud=$CARGA_MS"
      echo "hilos_rmc_por_vm=$HILOS_ESC"; echo "hilos_bd=$HILOS_BD"; echo "umbral_out=$UMBRAL_OUT"; echo "umbral_in=$UMBRAL_IN"
      echo "metrica=$METRICA"; echo "sostenido_out_s=$SOSTENIDO_OUT"; echo "sostenido_in_s=$SOSTENIDO_IN"; echo "enfriar_s=$ENFRIAR_S"; echo "cubeta=$CUBETA"; echo "fuga=$FUGA"; echo "asientos_por_ocurrencia=$ASIENTOS_ESC"
      echo "vm_gt=$IP_GT ($ET_GT)"; echo "vm_bd=$IP_BD ($ET_BD)"; echo "vm4=$IP_VM4 ($ET_VM4)"; echo "vm5=$IP_VM5 ($ET_VM5)"
      echo "generador=$ET_GEN"; } > "$SALIDA/config.txt"
    local i=0 fila nc npasos; npasos=$(echo $PASOS | wc -w)
    printf '\n%-8s %-9s %-22s %-22s %-22s %s\n' "Clientes" "ops/s" "CONSULTA prom/p95 ms" "RESERVA prom/p95 ms" "MODIFIC prom/p95 ms" "rechazos/errores"
    for n in $PASOS; do
        i=$((i + 1))
        fila=$(en "$IP_GT" "wc -l < logs/escala.csv" 2>/dev/null | tail -1)
        echo "$n,${fila:-0},$i" >> "$SALIDA/pasos.csv"
        # shellcheck disable=SC2046
        [ "$EN_VIVO" -gt 0 ] && echo "   >>> paso $i de $npasos: $n clientes durante ${PASO_S}s (solicitudes en vivo, una línea por segundo)" >&2
        gen_vivo "$SALIDA/paso${i}_c${n}.gen.log" $((PASO_S + 30)) --clientes "$n" --solo-consulta $((n / 4)) --pct-consulta 30 --pct-reserva 50 --pct-modificar 20 \
            --rpm "$RPM_ESC" --duracion "$PASO_S" --calentamiento 0 --gt "tcp://$IP_GT:5555" --notif "tcp://$IP_GT:5558" \
            --arquitectura escalamiento --repeticion "$i" --muestras "$SALIDA/paso${i}_muestras.csv"
        awk -v n="$n" -v dur="$PASO_S" -v rpm="$RPM_ESC" -v f="$SALIDA/pasos_resumen.csv" '
            /CONSULTA:/{split($0,a," "); for(k in a){if(a[k]~/^n=/)cn=substr(a[k],3); if(a[k]~/^prom=/)cp=substr(a[k],6); if(a[k]~/^p95=/)c95=substr(a[k],5)}}
            /RESERVA:/{for(k=1;k<=NF;k++){if($k~/^n=/)rn=substr($k,3); if($k~/^prom=/)rp=substr($k,6); if($k~/^p95=/)r95=substr($k,5)}}
            /MODIFIC/{for(k=1;k<=NF;k++){if($k~/^n=/)mn=substr($k,3); if($k~/^prom=/)mp=substr($k,6); if($k~/^p95=/)m95=substr($k,5)}}
            /errores=/{for(k=1;k<=NF;k++){if($k~/^errores=/)er=substr($k,9); if($k~/^sobrecarga=/)sb=substr($k,11)}}
            END{printf "%-8d %-9.0f %-22s %-22s %-22s %s/%s\n", n, (cn+rn+mn)/dur, cp"/"c95, rp"/"r95, mp"/"m95, sb+0, er+0
                printf "%d,%d,%.1f,%s,%s,%s,%d,%d\n", n, rpm, (cn+rn+mn)/dur, c95+0, r95+0, m95+0, sb+0, er+0 >> f}' "$SALIDA/paso${i}_c${n}.gen.log"
    done
    echo; echo "Reposo de ${REPOSO_S} s sin carga (el GT libera las VMs que ya no hacen falta)..."
    sleep "$REPOSO_S"
    wait "${SAMPLERS[@]}" 2>/dev/null
    en "$IP_GT" "cat logs/escala.csv" > "$SALIDA/escala_gt.csv" 2>/dev/null
    en "$IP_GT" "grep -E 'ESCALA|CUBETA|RMC registrado|sin señales' logs/gt1.log" > "$SALIDA/eventos_gt.txt" 2>/dev/null
    echo; echo "== Eventos del GT (escalado y cubeta) =="; grep -E "ESCALA|CUBETA" "$SALIDA/eventos_gt.txt" | sed 's/^/   /'
    if python3 -c "import pandas, matplotlib" 2>/dev/null; then
        python3 analisis/escalamiento.py "$SALIDA" --umbral-out "$UMBRAL_OUT" --umbral-in "$UMBRAL_IN" --paso-s $((PASO_S + 2)) ${TITULO_ESC:+--titulo "$TITULO_ESC"}
    else
        echo "(instale pandas y matplotlib para el gráfico:  pip3 install pandas matplotlib ; luego: python3 analisis/escalamiento.py $SALIDA)"
    fi
    if [ -n "$VEREDICTO_ESC" ]; then
        python3 analisis/veredicto.py "$SALIDA" "$VEREDICTO_ESC" --umbral-out "$UMBRAL_OUT" || true
    fi
    echo; echo "Listo. Archivos en $SALIDA/ . Guarde también la salida de esta ejecución."
    echo "(La topología sigue en marcha; para detenerla:  scripts/prueba_empirica.sh parar)"
}

# test1: la carga cabe en UNA VM (VM3): no debe haber scale-out ni cubeta, y VM4/VM5 no reciben trabajo.
cmd_test1() {
    SALIDA_ESC=${SALIDA_T1:-resultados_test1}
    PASOS=${T1_PASOS:-"3 6 9"}; PASO_S=${T1_PASO_S:-10}; RPM_ESC=${T1_RPM:-600}; REPOSO_S=${T1_REPOSO_S:-5}
    SOSTENIDO_IN=${T_SOSTENIDO_IN:-5}; ENFRIAR_S=${T_ENFRIAR_S:-5}   # escalado más ágil (3x más rápido que el 10 s / 8 s de 'escalamiento')
    CARGA_MS=${CARGA_T:-10}; HILOS_ESC=${HILOS_T:-8}
    TITULO_ESC="test1: una VM es suficiente para el tráfico"; VEREDICTO_ESC=test1
    echo "== test1: carga ligera (clientes $PASOS a ${RPM_ESC} solicitudes/min cada uno); debe atenderla solo VM3 =="
    cmd_escalamiento
}

# test2: la carga crece hasta ocupar las 3 VMs: scale-out a vm4 y vm5, CPU al límite y leaky bucket.
cmd_test2() {
    SALIDA_ESC=${SALIDA_T2:-resultados_test2}
    PASOS=${T2_PASOS:-"2 4 8 12 16 24 32 48 64 96 128"}; PASO_S=${T2_PASO_S:-10}; RPM_ESC=0; REPOSO_S=${T2_REPOSO_S:-20}
    SOSTENIDO_IN=${T_SOSTENIDO_IN:-5}; ENFRIAR_S=${T_ENFRIAR_S:-5}   # escalado más ágil (3x más rápido que el 10 s / 8 s de 'escalamiento')
    CARGA_MS=${CARGA_T:-10}; HILOS_ESC=${HILOS_T:-8}; CUBETA=${CUBETA_T2:-40}   # cubeta chica: con 24 trabajadores ocupados rechaza desde ~64 clientes
    TITULO_ESC="test2: carga creciente hasta ocupar las 3 VMs"; VEREDICTO_ESC=test2
    echo "== test2: rampa de clientes $PASOS sin pausa entre solicitudes; deben activarse vm4 y vm5 y la cubeta =="
    cmd_escalamiento
}

case "${1:-}" in
    mapa) cmd_mapa ;;
    escalamiento) cmd_escalamiento ;;
    test1) cmd_test1 ;;
    test2) cmd_test2 ;;
    procedimientos) cmd_procedimientos "${2:-}" ;;
    verificar) cmd_verificar ;;
    correr) cmd_correr "${2:-}" ;;
    parar) parar_todo; echo "Procesos del SRB detenidos en: $HOSTS" ;;
    *) awk 'NR>1 && /^#/{print; next} NR>1{exit}' "$0"; exit 2 ;;
esac
