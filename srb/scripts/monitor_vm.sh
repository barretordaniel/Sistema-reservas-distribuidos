#!/usr/bin/env bash
# Monitor en vivo de UNA VM de la capa lógica (VM3, VM4 o VM5): cuánta CPU y RAM usa, y cuánto de esa CPU es de cada
# componente del SRB. Una línea por segundo; se puede dejar abierto en una terminal mientras corre la prueba.
#
# Uso:  scripts/monitor_vm.sh [segundos_entre_muestras=1] [--csv archivo] [--n muestras]
#   CPU%   = % de TODOS los núcleos de la VM (100 = todos ocupados)       RAM% = memoria usada del total
#   gestor / rmc / canc = parte de la CPU que consume cada proceso (en % de todos los núcleos)
#   vivos = procesos del SRB que están corriendo en esta VM: G = gestor, R = rmc, C = cancelaciones
#   carga = load average de 1 minuto
# Toda la aritmética es entera en bash (no depende de awk). Ctrl+C para terminar.
export LC_ALL=C
INTERVALO=1; CSV=""; MAXN=0
while [ $# -gt 0 ]; do
    case $1 in
        --csv) CSV=$2; shift 2 ;;
        --n) MAXN=$2; shift 2 ;;
        *[!0-9]*|"") echo "Uso: $0 [segundos] [--csv archivo] [--n muestras]"; exit 2 ;;
        *) INTERVALO=$1; shift ;;
    esac
done
NUCLEOS=$(nproc 2>/dev/null || echo 1)
HOST=$(hostname)

declare -A previo   # pid -> ticks (usuario+sistema) de la muestra anterior
declare -A suma     # nombre -> ticks de esta muestra
declare -A nuevo

total_cpu() {  # deja en B (ocupado) y T (total) los jiffies de toda la máquina
    local x u n s i w irq sirq st
    read -r x u n s i w irq sirq st _ < /proc/stat
    B=$((u + n + s + irq + sirq + st)); T=$((B + i + w))
}
ram() {  # deja en RAM_PCT, RAM_USADA_MB y RAM_TOTAL_MB
    local k v tot=0 av=0
    while read -r k v _; do
        case $k in MemTotal:) tot=$v ;; MemAvailable:) av=$v ;; esac
    done < /proc/meminfo
    RAM_TOTAL_MB=$((tot / 1024)); RAM_USADA_MB=$(((tot - av) / 1024))
    RAM_PCT=$(( tot > 0 ? (tot - av) * 100 / tot : 0 ))
}
procesos() {  # ticks por componente: llena suma[] y nuevo[] (pid -> ticks) y arma VIVOS (G R C)
    local p c st
    suma=([gestor]=0 [rmc]=0 [cancelaciones]=0 [persistencia]=0 [postgres]=0)
    nuevo=(); VIVOS=""; local vg="-" vr="-" vc="-"
    for p in /proc/[0-9]*; do
        [ -r "$p/comm" ] || continue
        read -r c < "$p/comm" 2>/dev/null || continue
        case $c in
            gestor|rmc|cancelaciones|persistencia|postgres)
                read -r -a st < "$p/stat" 2>/dev/null || continue
                local pid=${p#/proc/} tk=$((st[13] + st[14]))
                nuevo[$pid]=$tk
                suma[$c]=$(( ${suma[$c]} + tk - ${previo[$pid]:-$tk} ))   # un proceso nuevo cuenta desde cero
                case $c in gestor) vg=G ;; rmc) vr=R ;; cancelaciones) vc=C ;; esac
                ;;
        esac
    done
    VIVOS="$vg$vr$vc"
    previo=()
    for p in "${!nuevo[@]}"; do previo[$p]=${nuevo[$p]}; done
}
pct() { local v=$(( $1 * 100 / ($2 > 0 ? $2 : 1) )); [ "$v" -gt 100 ] && v=100; echo $v; }
barra() { local n=$(( $1 / 5 )) i s=""; for ((i = 0; i < 20; i++)); do [ $i -lt $n ] && s+="#" || s+="."; done; echo "[$s]"; }

[ -n "$CSV" ] && [ ! -s "$CSV" ] && echo "hora,cpu_pct,ram_pct,gestor_pct,rmc_pct,cancelaciones_pct,vivos" > "$CSV"
echo "== $HOST: $NUCLEOS núcleos. Ctrl+C para terminar. CPU% = de todos los núcleos =="
total_cpu; b0=$B; t0=$T; procesos > /dev/null
n=0
while :; do
    sleep "$INTERVALO"
    total_cpu; db=$((B - b0)); dt=$((T - t0)); b0=$B; t0=$T
    procesos; ram
    cpu=$(pct "$db" "$dt")
    g=$(pct "${suma[gestor]}" "$dt"); r=$(pct "${suma[rmc]}" "$dt"); c=$(pct "${suma[cancelaciones]}" "$dt")
    read -r carga _ < /proc/loadavg
    if [ $((n % 20)) -eq 0 ]; then
        printf '%-8s %-5s %-22s %-5s %-6s %-6s %-6s %-9s %s\n' hora CPU% "" RAM% gestor rmc canc vivos carga
    fi
    printf '%-8s %4s%% %-22s %4s%% %5s%% %5s%% %5s%% %-9s %s\n' "$(date +%H:%M:%S)" "$cpu" "$(barra "$cpu")" \
        "$RAM_PCT" "$g" "$r" "$c" "$VIVOS" "$carga"
    [ -n "$CSV" ] && echo "$(date +%H:%M:%S),$cpu,$RAM_PCT,$g,$r,$c,$VIVOS" >> "$CSV"
    n=$((n + 1))
    [ "$MAXN" -gt 0 ] && [ "$n" -ge "$MAXN" ] && break
done
