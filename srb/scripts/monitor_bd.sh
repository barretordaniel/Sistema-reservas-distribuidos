#!/usr/bin/env bash
# Monitor en vivo de la VM de la BASE DE DATOS (VM2): cuántas peticiones se están ejecutando en PostgreSQL y cuánta RAM
# y CPU usa la VM. Una línea por segundo; se deja abierto en una terminal mientras corre la prueba.
#
# Uso:  scripts/monitor_bd.sh [segundos_entre_muestras=1] [--db srb] [--csv archivo] [--n muestras]
#   activas  = peticiones en curso: consultas/transacciones ejecutándose en la base, PROMEDIO del intervalo (20 fotos por
#              intervalo, porque cada petición dura ~1 ms);  max = el máximo visto en una foto
#   bloq     = de esas, las que esperan un lock (competencia por los mismos asientos)
#   conex    = conexiones abiertas a la base (las réplicas de Persistencia)
#   tx/s     = transacciones terminadas por segundo (COMMIT + ROLLBACK): el ritmo real de la BD
#   RAM%     = memoria usada del total de la VM;  pers/pg MB = memoria del proceso persistencia y de todos los postgres
#   CPU%     = de todos los núcleos de la VM (la BD suele ser el primer recurso en saturarse)
# Requiere poder ejecutar:  psql -d srb -c "select 1"   (el mismo usuario que corre Persistencia). Ctrl+C termina.
export LC_ALL=C
INTERVALO=1; CSV=""; MAXN=0; DB=${DB:-srb}
while [ $# -gt 0 ]; do
    case $1 in
        --csv) CSV=$2; shift 2 ;;
        --n) MAXN=$2; shift 2 ;;
        --db) DB=$2; shift 2 ;;
        *[!0-9]*|"") echo "Uso: $0 [segundos] [--db srb] [--csv archivo] [--n muestras]"; exit 2 ;;
        *) INTERVALO=$1; shift ;;
    esac
done
HOST=$(hostname)
NUCLEOS=$(nproc 2>/dev/null || echo 1)

total_cpu() { local x u n s i w irq sirq st; read -r x u n s i w irq sirq st _ < /proc/stat; B=$((u + n + s + irq + sirq + st)); T=$((B + i + w)); }
ram() {
    local k v tot=0 av=0
    while read -r k v _; do case $k in MemTotal:) tot=$v ;; MemAvailable:) av=$v ;; esac; done < /proc/meminfo
    RAM_PCT=$(( tot > 0 ? (tot - av) * 100 / tot : 0 )); RAM_TOTAL_MB=$((tot / 1024))
}
rss_mb() {  # rss_mb <nombre del proceso>: suma de la memoria residente (MB) de todos los procesos con ese nombre
    local p c st kb=0 pg
    pg=$(getconf PAGESIZE 2>/dev/null || echo 4096)
    for p in /proc/[0-9]*; do
        read -r c < "$p/comm" 2>/dev/null || continue
        [ "$c" = "$1" ] || continue
        read -r -a st < "$p/stat" 2>/dev/null || continue
        kb=$((kb + st[23] * pg / 1024))
    done
    echo $((kb / 1024))
}
consulta() {  # promedio y máximo de consultas activas, promedio de bloqueadas, conexiones y transacciones acumuladas
    # Las peticiones duran ~1 ms, así que una sola foto por segundo casi nunca las ve en ejecución. Por eso se toman
    # 20 fotos de pg_stat_activity repartidas en el intervalo (dentro de una sola sesión) y se promedian.
    local n=$((INTERVALO * 20))
    psql -d "$DB" -X -q -At -F'|' 2>&1 <<SQL
create function pg_temp.muestreo(n int) returns text language plpgsql as \$\$
declare i int; a int; b int; c int; sa int := 0; sb int := 0; mx int := 0; cx int := 0;
begin
  for i in 1..n loop
    perform pg_stat_clear_snapshot();
    select count(*) filter (where state = 'active'), count(*) filter (where wait_event_type = 'Lock'), count(*)
      into a, b, c from pg_stat_activity where datname = current_database() and pid <> pg_backend_pid();
    sa := sa + a; sb := sb + b; if a > mx then mx := a; end if; cx := c;
    perform pg_sleep(0.05);
  end loop;
  return round(sa::numeric / n, 1) || '|' || mx || '|' || round(sb::numeric / n, 1) || '|' || cx;
end \$\$;
select pg_temp.muestreo($n) || '|' || (select coalesce(xact_commit + xact_rollback, 0) from pg_stat_database
                                       where datname = current_database());
SQL
}

if ! command -v psql > /dev/null; then echo "No encuentro psql en esta VM (¿es la VM de la BD?)"; exit 1; fi
primera=$(INTERVALO=1 consulta)
case $primera in
    *"|"*"|"*"|"*"|"*) ;;
    *) echo "No pude consultar la base '$DB': $primera"; echo "Pruebe a mano:  psql -d $DB -c 'select 1'"; exit 1 ;;
esac
IFS='|' read -r _ _ _ _ tx0 <<< "$primera"
ahora_ms() { if [ -n "${EPOCHREALTIME:-}" ]; then local t=${EPOCHREALTIME/./}; echo $((t / 1000)); else echo $(($(date +%s) * 1000)); fi; }
t_ant=$(ahora_ms)
[ -n "$CSV" ] && [ ! -s "$CSV" ] && echo "hora,activas_prom,activas_max,bloqueadas,conexiones,tx_s,ram_pct,cpu_pct,persistencia_mb,postgres_mb" > "$CSV"
ram
echo "== $HOST (BD '$DB'): $NUCLEOS núcleos, RAM total $RAM_TOTAL_MB MB. Ctrl+C para terminar =="
total_cpu; b0=$B; t0=$T; n=0
while :; do
    r=$(consulta)   # tarda ~INTERVALO s: es el propio muestreo
    t_nue=$(ahora_ms); ms=$((t_nue - t_ant)); [ "$ms" -lt 100 ] && ms=100; t_ant=$t_nue
    IFS='|' read -r act amx blq con tx1 <<< "$r"
    total_cpu; db=$((B - b0)); dt=$((T - t0)); b0=$B; t0=$T
    cpu=$(( dt > 0 ? db * 100 / dt : 0 ))
    case $tx1 in   # se descuentan ~2 tx/s que genera el propio monitor al consultar la base
        *[!0-9]*|"") txs="?" ;;
        *) txs=$(( (tx1 - tx0) * 1000 / ms - 2 )); [ "$txs" -lt 0 ] && txs=0; tx0=$tx1 ;;
    esac
    ram; pers=$(rss_mb persistencia); pg=$(rss_mb postgres)
    if [ $((n % 20)) -eq 0 ]; then
        printf '%-8s %-8s %-6s %-6s %-6s %-7s %-5s %-5s %-9s %s\n' hora activas max bloq conex tx/s RAM% CPU% pers.MB pg.MB
    fi
    printf '%-8s %-8s %-6s %-6s %-6s %-7s %4s%% %4s%% %-9s %s\n' "$(date +%H:%M:%S)" "${act:-?}" "${amx:-?}" "${blq:-?}" "${con:-?}" "$txs" \
        "$RAM_PCT" "$cpu" "$pers" "$pg"
    [ -n "$CSV" ] && echo "$(date +%H:%M:%S),${act:-},${amx:-},${blq:-},${con:-},$txs,$RAM_PCT,$cpu,$pers,$pg" >> "$CSV"
    n=$((n + 1))
    [ "$MAXN" -gt 0 ] && [ "$n" -ge "$MAXN" ] && break
done
