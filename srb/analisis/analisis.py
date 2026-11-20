#!/usr/bin/env python3
"""Análisis de las corridas de carga del SRB (Tabla 1 del informe de rendimiento).

Entrada : uno o varios CSV de muestras generados por bin/generador (--muestras), uno por VM
          generadora y por corrida. Columnas:
          arquitectura,repeticion,clientes,vm_indice,tipo,ms,t_s,resultado,intentos,ventana_s
Salida  : resultados/tabla1.csv, resultados/tabla1.md y gráficos PNG en resultados/.

Uso:  python3 analisis/analisis.py resultados/*muestras*.csv [--salida resultados]

Método (igual que el enunciado):
  * cada corrida = (arquitectura, clientes, repeticion); sus muestras vienen de todas las VMs generadoras;
  * por corrida: promedio, desviación estándar y p95 del tiempo de respuesta (ms) por tipo de operación,
    y TPM = operaciones atendidas / minutos de la ventana medida (el calentamiento ya viene descartado);
  * por escenario (arquitectura x clientes): se promedian las corridas repetidas y se reporta
    la dispersión entre corridas.
"""
import argparse
import glob
import os
import sys

import pandas as pd

TIPOS = ["CONSULTA", "RESERVA", "MODIFICACION"]


def cargar(patrones):
    archivos = []
    for p in patrones:
        archivos.extend(glob.glob(p))
    if not archivos:
        sys.exit("No se encontró ningún CSV de muestras (¿ruta correcta?)")
    marcos = []
    for a in sorted(set(archivos)):
        df = pd.read_csv(a, comment="#")
        # tolera cabeceras repetidas si varios generadores escribieron el mismo archivo
        df = df[df["arquitectura"] != "arquitectura"]
        df["archivo"] = os.path.basename(a)
        marcos.append(df)
    df = pd.concat(marcos, ignore_index=True)
    for c in ["repeticion", "clientes", "vm_indice", "intentos"]:
        df[c] = pd.to_numeric(df[c])
    for c in ["ms", "t_s", "ventana_s"]:
        df[c] = pd.to_numeric(df[c])
    df["tipo"] = df["tipo"].replace({"MODIFIC.": "MODIFICACION"})
    return df


def p95(x):
    return x.quantile(0.95)


def por_corrida(df):
    """Una fila por (arquitectura, clientes, repeticion, tipo)."""
    clave = ["arquitectura", "clientes", "repeticion", "tipo"]
    filas = []
    for k, g in df.groupby(clave):
        ventana = g["ventana_s"].max()  # todas las VMs miden la misma ventana
        # el rechazo por negocio (p. ej. sin cupo) es una respuesta válida; el TIMEOUT/ERROR no entra en los tiempos
        ok = g[g["resultado"].isin(["EXITO", "RECHAZO"])]
        fila = dict(zip(clave, k))  # (sin el operador | de dict para que funcione con Python 3.8)
        fila.update(
            n=len(ok),
            promedio_ms=ok["ms"].mean(),
            std_ms=ok["ms"].std(ddof=0),
            p95_ms=p95(ok["ms"]),
            tpm=len(ok) / (ventana / 60.0) if ventana > 0 else float("nan"),
            errores=int((~g["resultado"].isin(["EXITO", "RECHAZO"])).sum()),
        )
        filas.append(fila)
    return pd.DataFrame(filas)


def por_escenario(pc):
    """Promedia las corridas repetidas de cada escenario."""
    agr = (
        pc.groupby(["arquitectura", "clientes", "tipo"])
        .agg(
            repeticiones=("repeticion", "nunique"),
            n=("n", "sum"),
            promedio_ms=("promedio_ms", "mean"),
            std_ms=("std_ms", "mean"),
            p95_ms=("p95_ms", "mean"),
            tpm=("tpm", "mean"),
            tpm_std_entre_corridas=("tpm", lambda s: s.std(ddof=0)),
            errores=("errores", "sum"),
        )
        .reset_index()
    )
    return agr


def tabla_formato_enunciado(esc):
    """Tabla 1: filas = arquitectura x clientes; columnas = métricas de consulta y de reserva."""
    filas = []
    for (arq, cl), g in esc.groupby(["arquitectura", "clientes"]):
        fila = {"Arquitectura": arq, "Clientes": cl}
        for tipo, etiqueta in [("CONSULTA", "Consulta"), ("RESERVA", "Reserva")]:
            t = g[g["tipo"] == tipo]
            if len(t):
                t = t.iloc[0]
                fila[f"{etiqueta} prom (ms)"] = round(t.promedio_ms, 2)
                fila[f"{etiqueta} desv (ms)"] = round(t.std_ms, 2)
                fila[f"{etiqueta} p95 (ms)"] = round(t.p95_ms, 2)
                fila[f"{etiqueta} TPM"] = round(t.tpm, 1)
        filas.append(fila)
    return pd.DataFrame(filas).sort_values(["Clientes", "Arquitectura"]).reset_index(drop=True)


def graficos(esc, salida):
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    colores = {"base": "#7a7a7a", "escalable": "#1f77b4"}
    arqs = sorted(esc["arquitectura"].unique())
    for metrica, titulo, archivo in [
        ("promedio_ms", "Tiempo de respuesta promedio (ms)", "tiempo_promedio.png"),
        ("p95_ms", "Tiempo de respuesta p95 (ms)", "tiempo_p95.png"),
        ("tpm", "Transacciones por minuto (TPM)", "tpm.png"),
    ]:
        fig, ejes = plt.subplots(1, 2, figsize=(9, 3.6), sharex=True)
        for ax, tipo in zip(ejes, ["CONSULTA", "RESERVA"]):
            for a in arqs:
                d = esc[(esc["arquitectura"] == a) & (esc["tipo"] == tipo)].sort_values("clientes")
                if d.empty:
                    continue
                ax.plot(d["clientes"], d[metrica], marker="o", label=a, color=colores.get(a))
            titulo_ax = tipo.capitalize()
            if metrica == "p95_ms":
                # la línea del objetivo (1 s) solo se dibuja si hay datos cerca de ella; si no, aplastaría la gráfica
                if esc[esc["tipo"] == tipo]["p95_ms"].max() > 300:
                    ax.axhline(1000, color="#c0392b", ls="--", lw=1)
                    ax.text(ax.get_xlim()[0], 1000, " objetivo p95 < 1 s", color="#c0392b", va="bottom", fontsize=8)
                elif esc[esc["tipo"] == tipo]["p95_ms"].max() > 0:
                    titulo_ax += " (todos los p95 < 1 s)"
            ax.set_title(titulo_ax)
            ax.set_ylim(bottom=0)
            ax.set_xlabel("clientes concurrentes")
            ax.set_xticks(sorted(esc["clientes"].unique()))
            ax.grid(alpha=0.3)
        ejes[0].set_ylabel(titulo)
        ejes[0].legend(title="arquitectura")
        fig.tight_layout()
        fig.savefig(os.path.join(salida, archivo), dpi=160)
        plt.close(fig)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", nargs="+", help="CSV de muestras (admite comodines)")
    ap.add_argument("--salida", default="resultados")
    a = ap.parse_args()
    os.makedirs(a.salida, exist_ok=True)

    df = cargar(a.csv)
    pc = por_corrida(df)
    esc = por_escenario(pc)
    tabla = tabla_formato_enunciado(esc)

    pc.to_csv(os.path.join(a.salida, "por_corrida.csv"), index=False)
    esc.to_csv(os.path.join(a.salida, "por_escenario.csv"), index=False)
    tabla.to_csv(os.path.join(a.salida, "tabla1.csv"), index=False)
    with open(os.path.join(a.salida, "tabla1.md"), "w", encoding="utf-8") as f:
        f.write(tabla.to_markdown(index=False) if _tiene_tabulate() else tabla.to_string(index=False))
        f.write("\n")
    graficos(esc, a.salida)

    pd.set_option("display.width", 200)
    pd.set_option("display.max_columns", 30)
    print("\n== Tabla 1 (promedio de las repeticiones de cada escenario) ==")
    print(tabla.to_string(index=False))
    print("\n== Corridas por escenario ==")
    print(pc.groupby(["arquitectura", "clientes"])["repeticion"].nunique().to_string())
    err = int(esc["errores"].sum())
    print(f"\nRespuestas con error/timeout en total: {err}")
    malos = esc[(esc["tipo"].isin(["CONSULTA", "RESERVA"])) & (esc["p95_ms"] >= 1000)]
    if len(malos):
        print("\nEscenarios que NO cumplen p95 < 1 s:")
        print(malos[["arquitectura", "clientes", "tipo", "p95_ms"]].to_string(index=False))
    else:
        print("Todos los escenarios cumplen p95 < 1 s.")
    print(f"\nArchivos escritos en {a.salida}/: tabla1.csv, tabla1.md, por_corrida.csv, por_escenario.csv, *.png")


def _tiene_tabulate():
    try:
        import tabulate  # noqa: F401

        return True
    except ImportError:
        return False


if __name__ == "__main__":
    main()
