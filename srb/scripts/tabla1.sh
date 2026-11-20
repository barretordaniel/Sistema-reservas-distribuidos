#!/usr/bin/env bash
# Controlador de las corridas de la Tabla 1 (base vs escalable; 3, 5 y 8 clientes; N repeticiones).
#
# Se ejecuta UNA vez, en una máquina que pueda hacer ssh (sin contraseña) a las demás.
# Por cada corrida:
#   1) restablece la BD al estado inicial (SIGUSR2 al servicio de persistencia),
#   2) pone en cero los contadores del GT (SIGUSR1),
#   3) lanza el generador en 1 VM (arquitectura base) o repartido en 2 VMs (escalable),
#   4) pide al GT su resumen (SIGUSR1) y trae los CSV de muestras a ./resultados/.
# Al final: python3 analisis/analisis.py resultados/*muestras*.csv
#
# Variables (todas con valor por defecto para la prueba local):
#   MODO=local|ssh        local = todo en esta máquina (solo para ensayar el script)
#   ARQ=base|escalable    arquitectura que se está midiendo
#   CLIENTES="3 5 8"      escenarios;  REPS=3 repeticiones;  DUR=120 s medidos;  CAL=15 s de calentamiento
#   GT_EPS=tcp://ip_gt:5555[,tcp://ip_relevo:6555]    endpoints de clientes de los GT
#   NOTIF=tcp://ip_cancelaciones:5558                 PUB de notificaciones
#   GEN0=usuario@ip_vm1  GEN1=usuario@ip_vm5          VMs generadoras (base usa solo GEN0; GENERADORES=1|2 lo fuerza)
#   BD_HOST=usuario@ip_vm3  GT_HOST=usuario@ip_vm2    dónde corren persistencia y GT principal
#   RUTA=~/srb            carpeta del proyecto en TODAS las VMs
set -u
MODO=${MODO:-local}
ARQ=${ARQ:-escalable}
CLIENTES=${CLIENTES:-"3 5 8"}
REPS=${REPS:-3}
DUR=${DUR:-120}
CAL=${CAL:-15}
PAUSA=${PAUSA:-5}
RPM=${RPM:-0}                 # 0 = cada cliente pide en cuanto recibe la respuesta anterior
GT_EPS=${GT_EPS:-tcp://localhost:5555}
NOTIF=${NOTIF:-tcp://localhost:5558}
GEN0=${GEN0:-local}; GEN1=${GEN1:-local}
BD_HOST=${BD_HOST:-local}; GT_HOST=${GT_HOST:-local}
RUTA=${RUTA:-$HOME/srb}
SALIDA=${SALIDA:-resultados}
cd "$(dirname "$0")/.."
mkdir -p "$SALIDA"

# ejecutar <host> <comando>   (local: sin ssh)
ejecutar() {
    local host=$1; shift
    if [ "$MODO" = "local" ] || [ "$host" = "local" ]; then bash -c "cd $PWD && $*"
    else ssh -o BatchMode=yes "$host" "cd $RUTA && $*"; fi
}
traer() {   # traer <host> <remoto> <local>
    if [ "$MODO" = "local" ] || [ "$1" = "local" ]; then [ "$2" = "$3" ] || cp "$2" "$3"
    else scp -q "$1:$RUTA/$2" "$3"; fi
}

# Perfil de carga del enunciado
#   3 clientes: 2 solo consulta + 1 consulta/reserva   -> 50% consulta, 50% reserva
#   5 y 8     : 3 (o 4) solo consulta, el resto reserva + modificación -> 70% reserva, 30% modificación
perfil() {
    case $1 in
        3) echo "--solo-consulta 2 --pct-consulta 50 --pct-reserva 50 --pct-modificar 0" ;;
        5) echo "--solo-consulta 3 --pct-consulta 0 --pct-reserva 70 --pct-modificar 30" ;;
        8) echo "--solo-consulta 4 --pct-consulta 0 --pct-reserva 70 --pct-modificar 30" ;;
        *) echo "--solo-consulta $(( $1 / 2 )) --pct-consulta 0 --pct-reserva 70 --pct-modificar 30" ;;
    esac
}

# VMs generadoras: 1 en la arquitectura base y 2 en la escalable (el trabajo se reparte por igual).
# Se puede forzar con GENERADORES=1 o 2 si una sola VM alcanza (mire con top que no pase de ~70% de CPU).
if [ -n "${GENERADORES:-}" ]; then VMS=$GENERADORES; elif [ "$ARQ" = "base" ]; then VMS=1; else VMS=2; fi
echo "Arquitectura=$ARQ  generadores=$VMS  clientes=[$CLIENTES]  repeticiones=$REPS  ${DUR}s medidos + ${CAL}s de calentamiento"
echo "GT=$GT_EPS  modo=$MODO"
echo

for N in $CLIENTES; do
  for R in $(seq 1 "$REPS"); do
    ETQ="${ARQ}_c${N}_r${R}"
    echo "== corrida $ETQ =="
    # 1) BD en el estado inicial y contadores del GT en cero
    # (el patrón [b]in evita que pkill se encuentre a sí mismo en la línea de comandos del shell)
    ejecutar "$BD_HOST" "pkill -USR2 -f '[b]in/persistencia' || echo 'AVISO: no encontré el servicio de persistencia en $BD_HOST'"
    sleep 4
    ejecutar "$GT_HOST" "pkill -USR1 -f '[b]in/gestor' >/dev/null 2>&1; true"
    sleep 1
    # 2) generadores (en paralelo)
    PIDS=()
    for i in $(seq 0 $((VMS-1))); do
        host=$GEN0; [ "$i" = "1" ] && host=$GEN1
        ejecutar "$host" "mkdir -p $SALIDA && bin/generador --clientes $N $(perfil "$N") --rpm $RPM \
            --duracion $((DUR+CAL)) --calentamiento $CAL --gt $GT_EPS --notif $NOTIF \
            --vm-indice $i --vm-total $VMS --arquitectura $ARQ --repeticion $R \
            --muestras $SALIDA/${ETQ}_vm${i}_muestras.csv --csv $SALIDA/${ETQ}_vm${i}_resumen.csv \
            > $SALIDA/${ETQ}_vm${i}.log 2>&1" &
        PIDS+=($!)
    done
    wait "${PIDS[@]}"
    # 3) resumen del GT y recogida de archivos
    ejecutar "$GT_HOST" "pkill -USR1 -f '[b]in/gestor' >/dev/null 2>&1; true"
    for i in $(seq 0 $((VMS-1))); do
        host=$GEN0; [ "$i" = "1" ] && host=$GEN1
        traer "$host" "$SALIDA/${ETQ}_vm${i}_muestras.csv" "$SALIDA/${ETQ}_vm${i}_muestras.csv"
        traer "$host" "$SALIDA/${ETQ}_vm${i}.log" "$SALIDA/${ETQ}_vm${i}.log"
        grep -E "CONSULTA:|RESERVA:|errores=" "$SALIDA/${ETQ}_vm${i}.log" | sed 's/^/   vm'"$i"' /'
    done
    sleep "$PAUSA"
  done
done

echo
echo "Listo. Archivos en $SALIDA/. Siguiente paso:"
echo "  python3 analisis/analisis.py '$SALIDA/*muestras*.csv' --salida $SALIDA"
echo "Y guarda el CSV del GT (--stats-csv) y 'vmstat 5' / 'top' de cada VM como evidencia de uso de recursos."
