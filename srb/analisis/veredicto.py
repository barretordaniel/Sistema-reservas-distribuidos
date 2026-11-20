#!/usr/bin/env python3
"""Veredicto automático de test1 y test2 (scripts/prueba_empirica.sh test1|test2).

Uso:  python3 analisis/veredicto.py resultados_test1 test1 [--umbral-out 50]
      python3 analisis/veredicto.py resultados_test2 test2 [--umbral-out 50]

Lee de la carpeta de resultados: escala_gt.csv (línea de tiempo del GT, una fila por segundo), pasos.csv y
pasos_resumen.csv (qué hizo cada paso de la rampa), config.txt y cpu_BD.csv. Imprime el veredicto y lo guarda en
veredicto.md. Cada comprobación sale como [OK] o [ATENCIÓN] con el número que la decide.
Compatible con Python 3.6+.
"""
import argparse
import os
import sys

import pandas as pd


def leer(ruta):
    if not os.path.exists(ruta) or os.path.getsize(ruta) == 0:
        return None
    try:
        return pd.read_csv(ruta)
    except Exception:
        return None


def leer_config(ruta):
    cfg = {}
    if os.path.exists(ruta):
        for linea in open(ruta, encoding="utf-8", errors="replace"):
            if "=" in linea:
                k, v = linea.rstrip("\n").split("=", 1)
                cfg[k] = v
    return cfg


def secuencia(niveles):
    sec = []
    for n in niveles:
        if not sec or sec[-1] != n:
            sec.append(int(n))
    return sec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("carpeta")
    ap.add_argument("prueba", choices=["test1", "test2"])
    ap.add_argument("--umbral-out", type=float, default=50.0)
    a = ap.parse_args()
    d = a.carpeta

    gt = leer(os.path.join(d, "escala_gt.csv"))
    if gt is None or len(gt) == 0:
        sys.exit("No hay %s/escala_gt.csv (o está vacío): la prueba no produjo la línea de tiempo del GT." % d)
    pasos = leer(os.path.join(d, "pasos.csv"))
    res = leer(os.path.join(d, "pasos_resumen.csv"))
    bd = leer(os.path.join(d, "cpu_BD.csv"))
    cfg = leer_config(os.path.join(d, "config.txt"))
    niveles = [c[4:] for c in gt.columns if c.startswith("cpu_")]
    t = gt["t_s"].astype(float)

    # ventana de la rampa (sin el reposo final)
    ini = 0.0
    fin = float(t.iloc[-1]) + 1
    if pasos is not None and len(pasos) > 0:
        ini = max(0.0, float(pasos["fila_gt"].iloc[0]) - 1.0)
        paso_s = float(cfg.get("paso_s", 30)) + 2.0
        fin = max(0.0, float(pasos["fila_gt"].iloc[-1]) - 1.0) + paso_s
    en = gt[(t >= ini) & (t < fin)]
    if len(en) == 0:
        en = gt

    comprobaciones = []  # (ok, texto)

    def chk(ok, texto):
        comprobaciones.append((bool(ok), texto))

    maxn = int(gt["niveles_activos"].max())
    sec = secuencia(gt["niveles_activos"])
    total_sobre = int(en["sobrecarga"].sum())
    cubeta_s = int(en["cubeta"].sum())
    errores = int(res["errores"].sum()) if res is not None and "errores" in res else 0
    p95 = 0.0
    if res is not None:
        for c in ("consulta_p95_ms", "reserva_p95_ms", "modificacion_p95_ms"):
            if c in res:
                p95 = max(p95, float(res[c].max()))
    cpu_max = {n: float(en["cpu_" + n].max()) for n in niveles}
    cpu_med = {n: float(en["cpu_" + n].mean()) for n in niveles}

    lineas = []
    tabla = []
    if a.prueba == "test1":
        titulo = "test1: una sola VM es suficiente para el tráfico"
        base = niveles[0]
        otros = [n for n in niveles[1:]]
        trabajo_otros = sum(float(en["desp_" + n].sum()) for n in otros)
        trabajo_base = float(en["desp_" + base].sum())
        chk(maxn == 1, "no hubo scale-out: siempre 1 nivel activo (máximo %d)" % maxn)
        chk(cpu_max[base] < a.umbral_out,
            "la CPU de %s se mantuvo bajo el umbral de scale-out: pico %.0f %%, media %.0f %% (umbral %.0f %%)" %
            (base, cpu_max[base], cpu_med[base], a.umbral_out))
        chk(trabajo_otros == 0 and trabajo_base > 0,
            "todo el trabajo lo hizo %s (%d solicitudes); %s no recibieron ninguna (%d)" %
            (base, trabajo_base, "+".join(otros) if otros else "otras VMs", trabajo_otros))
        chk(cubeta_s == 0 and total_sobre == 0, "el leaky bucket no se activó y no hubo rechazos (%d)" % total_sobre)
        chk(errores == 0 and p95 < 1000, "sin errores (%d) y p95 máximo %.0f ms (< 1000 ms)" % (errores, p95))
        if res is not None and "rpm_cliente" in res:
            bien = True
            detalle = []
            for _, r in res.iterrows():
                ofrecido = float(r["clientes"]) * float(r["rpm_cliente"]) / 60.0
                if ofrecido > 0:
                    detalle.append("%d clientes: %.0f de %.0f/s" % (int(r["clientes"]), float(r["ops_s"]), ofrecido))
                    if float(r["ops_s"]) < 0.85 * ofrecido:
                        bien = False
            if detalle:
                chk(bien, "la VM sostuvo el ritmo ofrecido (>= 85 %%): %s" % "; ".join(detalle))
        nota = ("Si falla el umbral de CPU, la carga de test1 es demasiada para una VM: baje T1_RPM o T1_PASOS "
                "(por ejemplo T1_RPM=300).")
        aprobado = all(ok for ok, _ in comprobaciones)
    else:
        titulo = "test2: carga creciente hasta ocupar las 3 VMs"
        chk(sec[:3] == [1, 2, 3] if len(sec) >= 3 else False,
            "los niveles se activaron en orden 1 -> 2 -> 3 (secuencia vista: %s)" % " -> ".join(str(x) for x in sec))
        for n in niveles:
            serie = en["cpu_" + n].rolling(3, min_periods=3).mean()
            pico = float(serie.max()) if serie.notnull().any() else cpu_max[n]
            chk(pico >= 90, "%s llegó a la saturación: CPU sostenida (3 s) máxima %.0f %% (pico %.0f %%)" %
                (n, pico, cpu_max[n]))
        # capacidad por número de niveles activos: media de la mitad más alta de los segundos con ese nivel
        tp = {}
        for k in range(1, maxn + 1):
            g = en[en["niveles_activos"] == k]
            if len(g) >= 4:
                v = g["respondidas"].astype(float).sort_values(ascending=False)
                tp[k] = float(v.iloc[: max(2, len(v) // 2)].mean())
                fila = {"niveles": k, "segundos": len(g), "ops_s_pico": tp[k]}
                for n in niveles:
                    fila["cpu_" + n] = float(g["cpu_" + n].mean())
                tabla.append(fila)
        if 1 in tp and maxn in tp and maxn > 1:
            razon = tp[maxn] / tp[1] if tp[1] > 0 else 0
            chk(razon >= 1.5,
                "la capacidad crece al sumar VMs: %.0f ops/s con 1 nivel -> %.0f ops/s con %d (x%.1f; se espera >= x1.5)" %
                (tp[1], tp[maxn], maxn, razon))
        chk(cubeta_s > 0, "el leaky bucket se activó (%d s)" % cubeta_s)
        chk(total_sobre > 0 or cubeta_s == 0,
            "la cubeta rechazó el exceso con SOBRECARGA (%d solicitudes rechazadas; si es 0, la cola no llegó a llenarse: "
            "baje CUBETA_T2 o suba T2_PASOS)" % total_sobre)
        chk(errores == 0 and p95 < 1000,
            "sin errores (%d); p95 máximo de lo atendido %.0f ms (< 1000 ms gracias a la cubeta)" % (errores, p95))
        final = int(gt["niveles_activos"].iloc[-1])
        chk(final == 1, "en el reposo final se liberaron las VMs: queda %d nivel activo" % final)
        nota = ("Si una VM no llega al 90 %: suba CARGA_T (ms de CPU por solicitud) o T2_PASOS (más clientes). Si la BD "
                "llega a >= 85 %, el techo es de la BD y no de la capa lógica.")
        # criterio: lo esencial de test2 son el orden, la saturación de las 3 VMs, la cubeta y la ausencia de errores
        esencial = [ok for ok, txt in comprobaciones
                    if not txt.startswith("la capacidad") and not txt.startswith("la cubeta rechazó")]
        aprobado = all(esencial)

    if bd is not None and "cpu_pct" in bd:
        v = pd.to_numeric(bd["cpu_pct"], errors="coerce")
        v = v[(v >= 0) & (v <= 100)]
        if len(v):
            lineas.append("CPU de la BD (VM2): media %.0f %%, máxima %.0f %%%s." %
                          (v.mean(), v.max(), " -> la BD llegó a >= 85 %: es un límite del sistema" if v.max() >= 85 else ""))

    salida = ["# Veredicto de %s" % titulo, ""]
    if cfg:
        salida.append("Configuración: clientes %s, %s s por paso, %s solicitudes/min por cliente (0 = sin pausa), "
                      "CPU sintética %s ms por solicitud, %s hilos RMC por VM." %
                      (cfg.get("pasos", "?"), cfg.get("paso_s", "?"), cfg.get("rpm_por_cliente", "?"),
                       cfg.get("carga_cpu_ms_por_solicitud", "?"), cfg.get("hilos_rmc_por_vm", "?")))
        salida.append("")
    for ok, txt in comprobaciones:
        salida.append("- [%s] %s" % ("OK" if ok else "ATENCIÓN", txt))
    salida.append("")
    if tabla:
        cols = list(tabla[0].keys())
        salida.append("| " + " | ".join(cols) + " |")
        salida.append("|" + "|".join(["---"] * len(cols)) + "|")
        for fila in tabla:
            salida.append("| " + " | ".join(("%.0f" % fila[c]) if isinstance(fila[c], float) else str(fila[c]) for c in cols) + " |")
        salida.append("")
    salida += lineas
    if lineas:
        salida.append("")
    salida.append("CPU máxima por VM durante la rampa: " + ", ".join("%s %.0f %%" % (n, cpu_max[n]) for n in niveles))
    salida.append("")
    salida.append("**VEREDICTO %s: %s**" % (a.prueba.upper(), "APROBADO" if aprobado else "REVISAR (hay comprobaciones en ATENCIÓN)"))
    if not aprobado:
        salida.append(nota)
    texto = "\n".join(salida) + "\n"
    with open(os.path.join(d, "veredicto.md"), "w", encoding="utf-8") as f:
        f.write(texto)
    print("\n== Veredicto de %s ==" % titulo)
    print(texto)
    print("Guardado en %s/veredicto.md" % d)


if __name__ == "__main__":
    main()
