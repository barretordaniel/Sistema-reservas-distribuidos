#!/usr/bin/env bash
# Dice qué máquina es esta y qué papel cumple en el SRB. Córralo en CADA VM (no necesita configuración).
#
#   scripts/quien_soy.sh               informe completo
#   scripts/quien_soy.sh --corto       una sola línea (lo usa prueba_empirica.sh mapa)
#   scripts/quien_soy.sh --fijar VM4   anota el rol en ~/.srb_rol (para cuando todavía no hay procesos corriendo)
#
# El rol se deduce de lo que está corriendo y de los puertos en escucha; si no hay nada corriendo, usa el rol
# anotado con --fijar y, si tampoco existe, da pistas (PostgreSQL instalado, binarios compilados).
RUTA_ROL="$HOME/.srb_rol"
if [ "${1:-}" = "--fijar" ]; then
    [ -n "${2:-}" ] || { echo "Uso: $0 --fijar VM1|VM2|VM3|VM4|VM5"; exit 2; }
    echo "$2" > "$RUTA_ROL"; echo "Rol anotado en $RUTA_ROL: $2"; exit 0
fi
CORTO=0; [ "${1:-}" = "--corto" ] && CORTO=1

IPS=$(hostname -I 2>/dev/null | tr ' ' '\n' | grep -E '^[0-9]+\.' | paste -sd' ')
PROCS=$(pgrep -af 'bin/(persistencia|gestor|rmc|cancelaciones|generador|cliente)( |$)' | grep -v "quien_soy" || true)
if command -v ss >/dev/null 2>&1; then
    PUERTOS=$(ss -ltn 2>/dev/null | awk 'NR>1{n=split($4,a,":"); print a[n]}' | sort -un | tr '\n' ' ')
else  # sin ss: se leen los sockets en escucha (estado 0A) de /proc/net/tcp
    PUERTOS=$(for h in $(awk 'NR>1 && $4=="0A"{split($2,a,":"); print a[2]}' /proc/net/tcp /proc/net/tcp6 2>/dev/null); do echo $((16#$h)); done | sort -un | tr '\n' ' ')
fi
tiene() { echo "$PROCS" | grep -q "bin/$1"; }
escucha() { echo " $PUERTOS " | grep -q " $1 "; }
PG=0; pgrep -x postgres >/dev/null 2>&1 && PG=1

roles=()
{ [ "$PG" = 1 ] || tiene persistencia; } && roles+=("ALMACENAMIENTO (VM2: PostgreSQL + persistencia)")
if tiene gestor; then
    if echo "$PROCS" | grep -q -- "GT2\|6555"; then roles+=("GT DE RELEVO (puertos 6555/6556)")
    else roles+=("GT PRINCIPAL (VM3: entrada del negocio)"); fi
fi
tiene cancelaciones && roles+=("CANCELACIONES (VM3)")
for n in $(echo "$PROCS" | grep "bin/rmc" | grep -o -- "--nombre [^ ]*" | awk '{print $2}' | sort -u); do
    roles+=("RMC réplica '$n'")
done
echo "$PROCS" | grep -q "bin/rmc" && ! echo "$PROCS" | grep -q -- "--nombre" && roles+=("RMC (sin --nombre)")
tiene generador && roles+=("GENERADOR DE CARGA (VM1: consumidores)")
tiene cliente && roles+=("CLIENTE de archivo (VM1: consumidores)")

if [ "${#roles[@]}" -gt 0 ]; then ROL=$(printf '%s + ' "${roles[@]}"); ROL=${ROL% + }
elif [ -f "$RUTA_ROL" ]; then ROL="$(cat "$RUTA_ROL") (anotado con --fijar; no hay procesos del SRB corriendo)"
else
    ROL="sin procesos del SRB corriendo: no se puede deducir"
    if command -v psql >/dev/null 2>&1 && [ -d /var/lib/postgresql ]; then ROL="$ROL; tiene PostgreSQL instalado -> probablemente VM2 (la BD)"; fi
fi

if [ "$CORTO" = 1 ]; then
    echo "$(hostname) [$IPS] -> $ROL"; exit 0
fi

echo "=== Quién es esta máquina ==="
echo "Nombre      : $(hostname)"
echo "Usuario     : $(whoami)"
echo "Direcciones : ${IPS:-?}"
echo "CPU / RAM   : $(nproc) núcleos / $(free -h | awk '/Mem/{print $2}')"
echo "ROL         : $ROL"
echo
echo "--- Procesos del SRB corriendo ---"
[ -n "$PROCS" ] && echo "$PROCS" | sed 's/^/  /' || echo "  (ninguno)"
[ "$PG" = 1 ] && echo "  PostgreSQL: activo (puerto 5432: $(escucha 5432 && echo escuchando || echo 'sin escuchar en TCP; usa socket Unix'))"
echo
echo "--- Puertos del SRB en escucha ---"
for p in 5432:PostgreSQL 5555:GT-clientes 5556:GT-RMC 5557:Cancelaciones-PULL 5558:Cancelaciones-PUB 5559:Persistencia 6555:GT-relevo-clientes 6556:GT-relevo-RMC; do
    escucha "${p%%:*}" && echo "  ${p%%:*}  ${p#*:}"
done
echo "  (los RMC y el generador no escuchan: se conectan hacia los demás)"
echo
AQUI=$(cd "$(dirname "$0")" 2>/dev/null && pwd)
BIN=""
for c in "$PWD/bin" "$AQUI/bin" "$AQUI/../bin"; do   # el bin/ del proyecto: el que tenga algún binario del SRB
    for b in rmc gestor persistencia generador; do [ -x "$c/$b" ] && { BIN=$(cd "$c" && pwd); break 2; }; done
done
echo "--- Binarios compilados (en ${BIN:-ninguna carpeta bin/ del proyecto encontrada}) ---"
[ -n "$BIN" ] && ls "$BIN" | grep -E '^(persistencia|gestor|rmc|cancelaciones|generador|cliente)$' | tr '\n' ' '; echo
echo
echo "Papel de cada VM:  VM1 consumidores y control · VM2 PostgreSQL + persistencia · VM3 GT + cancelaciones + RMC 'base'"
echo "                   VM4 RMC 'vm4' (+ GT de relevo en la variante escalable) · VM5 RMC 'vm5'"
echo "Si esta máquina aún no corre nada y no sabe cuál es: anótelo con  scripts/quien_soy.sh --fijar VM4"
