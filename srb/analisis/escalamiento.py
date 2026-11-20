#!/usr/bin/env python3
"""Gráfico y resumen de la prueba de escalamiento (scripts/prueba_empirica.sh escalamiento).

Uso:  python3 analisis/escalamiento.py resultados_escalamiento [--umbral-out 50] [--umbral-in 20] [--paso-s 27]

Lee de la carpeta:  escala_gt.csv (línea de tiempo del GT, una fila por segundo), pasos.csv (cuándo empezó cada
paso de la rampa), cpu_VM1_generador.csv y cpu_BD.csv (CPU por segundo de VM1 y de la VM de la BD).
Escribe:  escalamiento.png  y  escalamiento_resumen.md  (y el resumen también por pantalla).
Compatible con Python 3.6+.
"""
import argparse
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import pandas as pd  # noqa: E402


def leer(ruta):
    if not os.path.exists(ruta) or os.path.getsize(ruta) == 0:
        return None
    try:
        return pd.read_csv(ruta)
    except Exception:
        return None


def leer_cpu(ruta, nombre, avisos):
    """CSV t_s,cpu_pct de una VM; descarta lo que no sea un porcentaje válido (0..100) y lo avisa."""
    d = leer(ruta)
    if d is None or "cpu_pct" not in d.columns:
        return None
    d = d[pd.to_numeric(d["t_s"], errors="coerce").notnull()].copy()
    d["t_s"] = pd.to_numeric(d["t_s"])
    d["cpu_pct"] = pd.to_numeric(d["cpu_pct"], errors="coerce")
    malas = int(((d["cpu_pct"] < 0) | (d["cpu_pct"] > 100) | d["cpu_pct"].isnull()).sum())
    d.loc[(d["cpu_pct"] < 0) | (d["cpu_pct"] > 100), "cpu_pct"] = float("nan")
    if malas:
        avisos.append("AVISO: %d de %d muestras de CPU de %s no eran un porcentaje válido (se ignoran). "
                      "Si son casi todas, la medición de esa VM no es confiable: pegue cpu_%s.csv." %
                      (malas, len(d), nombre, "BD" if nombre.startswith("la BD") else "VM1_generador"))
    if d["cpu_pct"].notnull().sum() == 0:
        return None
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("carpeta")
    ap.add_argument("--umbral-out", type=float, default=50.0)
    ap.add_argument("--umbral-in", type=float, default=20.0)
    ap.add_argument("--paso-s", type=float, default=0.0, help="duración de un paso (s) para delimitar el último")
    ap.add_argument("--titulo", default="Escalamiento por CPU: scale-out, scale-in y leaky bucket")
    a = ap.parse_args()
    d = a.carpeta

    gt = leer(os.path.join(d, "escala_gt.csv"))
    if gt is None or len(gt) == 0:
        sys.exit("No hay %s/escala_gt.csv (o está vacío): ¿corrió el GT con --linea-tiempo?" % d)
    pasos = leer(os.path.join(d, "pasos.csv"))
    avisos = []
    vm1 = leer_cpu(os.path.join(d, "cpu_VM1_generador.csv"), "VM1", avisos)
    bd = leer_cpu(os.path.join(d, "cpu_BD.csv"), "la BD (VM2)", avisos)

    niveles = [c[4:] for c in gt.columns if c.startswith("cpu_")]
    t = gt["t_s"].astype(float)

    # inicio de cada paso en el eje del GT (la fila del CSV = segundo desde que arrancó el GT)
    inicios = []
    if pasos is not None and len(pasos) > 0:
        for _, r in pasos.iterrows():
            inicios.append((max(0.0, float(r["fila_gt"]) - 1.0), int(r["clientes"])))
    if inicios:
        difs = sorted(inicios[k + 1][0] - inicios[k][0] for k in range(len(inicios) - 1))
        paso_s = a.paso_s if a.paso_s > 0 else (difs[len(difs) // 2] if difs else 30.0)
        fin_rampa = inicios[-1][0] + paso_s  # donde termina la rampa; después viene el reposo (sin carga)
    else:
        paso_s, fin_rampa = 30.0, float(t.iloc[-1]) + 1
    off = inicios[0][0] - 1.0 if inicios else 0.0  # las series de VM1 y BD empiezan ~1 s antes del primer paso

    fig, ax = plt.subplots(4, 1, figsize=(11, 11), sharex=True)
    colores_n = {1: "#dbe9f6", 2: "#bcd8ee", 3: "#8fc0e0"}
    paleta = ["#1f77b4", "#2ca02c", "#d62728", "#9467bd", "#8c564b"]

    # --- 1. CPU de cada nivel y umbrales
    for i, n in enumerate(niveles):
        ax[0].plot(t, gt["cpu_" + n], label="CPU " + n, color=paleta[i % len(paleta)], lw=1.8)
    ax[0].axhline(a.umbral_out, color="red", ls="--", lw=1, label="umbral scale-out (%d%%)" % a.umbral_out)
    ax[0].axhline(a.umbral_in, color="gray", ls=":", lw=1, label="umbral scale-in (%d%%)" % a.umbral_in)
    ax[0].set_ylabel("CPU de la VM (%)")
    ax[0].set_ylim(0, 105)
    ax[0].set_title(a.titulo)
    # franjas: niveles activos
    nivel_act = gt["niveles_activos"].astype(int).tolist()
    for k in range(len(t)):
        ax[0].axvspan(t.iloc[k] - 0.5, t.iloc[k] + 0.5, color=colores_n.get(nivel_act[k], "#8fc0e0"), alpha=0.45, lw=0)
    ax[0].plot([], [], color=colores_n[1], lw=8, label="1 VM activa")
    ax[0].plot([], [], color=colores_n[2], lw=8, label="2 VMs activas")
    ax[0].plot([], [], color=colores_n[3], lw=8, label="3 VMs activas")
    ax[0].legend(loc="upper left", ncol=3, fontsize=8)

    # --- 2. throughput, rechazos y clientes
    ax[1].plot(t, gt["respondidas"], color="#1f77b4", label="solicitudes atendidas /s", lw=1.5)
    ax[1].plot(t, gt["sobrecarga"], color="#d62728", label="rechazadas por SOBRECARGA /s", lw=1.5)
    for k in range(len(t)):
        if int(gt["cubeta"].iloc[k]) == 1:
            ax[1].axvspan(t.iloc[k] - 0.5, t.iloc[k] + 0.5, color="#f4c7c3", alpha=0.6, lw=0)
    ax[1].plot([], [], color="#f4c7c3", lw=8, label="leaky bucket activo")
    ax[1].set_ylabel("solicitudes por segundo")
    ax1b = ax[1].twinx()
    if inicios:
        xs, ys = [], []
        for k, (ini, nc) in enumerate(inicios):
            fin = inicios[k + 1][0] if k + 1 < len(inicios) else fin_rampa
            xs += [ini, fin]
            ys += [nc, nc]
        xs += [fin_rampa, float(t.iloc[-1])]
        ys += [0, 0]
        ax1b.step(xs, ys, where="post", color="black", lw=1, alpha=0.7)
    ax1b.set_ylabel("clientes concurrentes", color="black")
    h1, l1 = ax[1].get_legend_handles_labels()
    ax[1].legend(h1, l1, loc="upper left", fontsize=8)

    # --- 3. reparto por VM
    desp = [gt["desp_" + n] for n in niveles]
    ax[2].stackplot(t, *desp, labels=["trabajo en " + n for n in niveles], colors=paleta[: len(niveles)], alpha=0.8)
    ax[2].set_ylabel("solicitudes despachadas /s")
    ax[2].legend(loc="upper left", fontsize=8, ncol=len(niveles))

    # --- 4. BD y generador
    if bd is not None:
        ax[3].plot(bd["t_s"] + off, bd["cpu_pct"], color="#ff7f0e", label="CPU de la BD (VM2)", lw=1.5)
    if vm1 is not None:
        ax[3].plot(vm1["t_s"] + off, vm1["cpu_pct"], color="#7f7f7f", label="CPU del generador (VM1)", lw=1.5)
    ax[3].axhline(85, color="red", ls=":", lw=1, label="85 % (saturada)")
    ax[3].set_ylabel("CPU (%)")
    ax[3].set_ylim(0, 105)
    ax[3].set_xlabel("segundos desde el arranque del GT")
    ax[3].legend(loc="upper left", fontsize=8, ncol=3)
    ax[3].set_xlim(float(t.iloc[0]), float(t.iloc[-1]))
    fig.tight_layout()
    png = os.path.join(d, "escalamiento.png")
    fig.savefig(png, dpi=130)

    # --- resumen por paso
    filas = []
    ventanas = [(ini, inicios[k + 1][0] if k + 1 < len(inicios) else fin_rampa, nc) for k, (ini, nc) in enumerate(inicios)]
    if inicios and float(t.iloc[-1]) - fin_rampa >= 5:  # reposo: sin carga (muestra la línea base y el scale-in final)
        ventanas.append((fin_rampa + 2, float(t.iloc[-1]) + 1, 0))
    for (ini, fin, nc) in ventanas:
        m = (t >= ini) & (t < fin)
        g = gt[m]
        if len(g) == 0:
            continue
        fila = {
            "clientes": nc,
            "ops_s": g["respondidas"].mean(),
            "rechazadas_s": g["sobrecarga"].mean(),
            "cola_max": int(g["cola"].max()),
            "niveles_fin": int(g["niveles_activos"].iloc[-1]),
            "cubeta": "sí" if int(g["cubeta"].max()) == 1 else "no",
        }
        for n in niveles:
            fila["cpu_" + n] = g["cpu_" + n].mean()
        if bd is not None:
            b = bd[((bd["t_s"] + off) >= ini) & ((bd["t_s"] + off) < fin)]
            fila["cpu_BD"] = b["cpu_pct"].mean() if b["cpu_pct"].notnull().any() else float("nan")
        if vm1 is not None:
            v = vm1[((vm1["t_s"] + off) >= ini) & ((vm1["t_s"] + off) < fin)]
            fila["cpu_VM1"] = v["cpu_pct"].mean() if v["cpu_pct"].notnull().any() else float("nan")
        filas.append(fila)
    res = pd.DataFrame(filas)

    lineas = ["# Escalamiento por CPU: resumen por paso de la rampa", "",
              "(clientes = 0: reposo al final, sin carga; muestra la CPU base de cada VM y el scale-in)", ""]
    if len(res):
        cols = list(res.columns)
        lineas.append("| " + " | ".join(cols) + " |")
        lineas.append("|" + "|".join(["---"] * len(cols)) + "|")
        for _, r in res.iterrows():
            celdas = []
            for c in cols:
                v = r[c]
                celdas.append(("%.1f" % v) if isinstance(v, float) else str(v))
            lineas.append("| " + " | ".join(celdas) + " |")
    lineas.append("")
    # --- lectura automática
    ultimo = gt.iloc[-1]
    maxn = int(gt["niveles_activos"].max())
    lineas.append("Niveles activos: máximo %d de %d; al final %d." % (maxn, len(niveles), int(ultimo["niveles_activos"])))
    cambios = (gt["niveles_activos"].diff().fillna(0) != 0).sum()
    lineas.append("Cambios de nivel (scale-out + scale-in): %d." % cambios)
    if gt["cubeta"].max() == 1:
        t_cub = int(gt["cubeta"].sum())
        lineas.append("Leaky bucket activo durante %d s; rechazadas por SOBRECARGA en total: %d." % (t_cub, int(gt["sobrecarga"].sum())))
    else:
        lineas.append("El leaky bucket no llegó a activarse (el último nivel nunca se mantuvo sobre el umbral).")
    for av in avisos:
        lineas.append(av)
    if bd is not None:
        lineas.append("CPU de la BD: media %.0f %%, máxima %.0f %%." % (bd["cpu_pct"].mean(), bd["cpu_pct"].max()))
        if bd["cpu_pct"].max() >= 85:
            lineas.append("  -> la BD llegó a >= 85 %: es el recurso que limita el rendimiento total del sistema.")
    if vm1 is not None:
        lineas.append("CPU del generador (VM1): máxima %.0f %%." % vm1["cpu_pct"].max())
        if vm1["cpu_pct"].max() >= 85:
            lineas.append("  -> VM1 se saturó: parte del límite medido es del generador, no del SRB.")
    for n in niveles:
        lineas.append("CPU máxima de %s: %.0f %%." % (n, gt["cpu_" + n].max()))
    texto = "\n".join(lineas) + "\n"
    with open(os.path.join(d, "escalamiento_resumen.md"), "w", encoding="utf-8") as f:
        f.write(texto)
    print("\n== Resumen del escalamiento ==")
    print(texto)
    print("Gráfico: %s" % png)


if __name__ == "__main__":
    main()
