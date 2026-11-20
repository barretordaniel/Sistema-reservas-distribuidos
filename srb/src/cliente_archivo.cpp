// cliente_archivo.cpp: cliente que ejecuta las operaciones de un archivo de texto contra el GT.
// Sirve para reproducir "el caso de prueba del profesor" (n operaciones concretas sobre eventos
// concretos), ver cada resultado y comparar la BD antes y después.
//
// Formato del archivo (una operación por línea; '#' inicia un comentario):
//   CONSULTA <mes AAAAMM>
//   RESERVA <cliente> <evento> <ocurrencia> <asientos>
//   MODIFICAR_ASIENTOS <cliente> <reserva> <evento> <ocurrencia> <asientos_actuales> <asientos_nuevos>
//   MODIFICAR_FECHA <cliente> <reserva> <evento> <ocurrencia> <asientos_actuales> <fecha_nueva AAAAMMDD>
//   CANCELAR <cliente> <reserva>          (asíncrona: se responde de inmediato y luego llega la notificación)
//   PAUSA <milisegundos>
// En <reserva> se puede escribir $ULTIMA: el id de la última reserva confirmada por este archivo.
//
// Uso: bin/cliente operaciones.txt [--gt tcp://ip:5555[,tcp://ip_relevo:6555]] [--notif tcp://ip:5558]
//                                   [--todo] [--esperar-notif 3000] [--pausa-ms 0]
//   --todo  imprime todos los eventos de cada consulta (por defecto solo un resumen)
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>

#include "common.hpp"

using namespace srb;

namespace {

std::vector<std::string> partir(const std::string& s, char sep) {
    std::vector<std::string> v;
    std::stringstream ss(s);
    std::string t;
    while (std::getline(ss, t, sep))
        if (!t.empty()) v.push_back(t);
    return v;
}

struct Operacion {
    std::string linea;  // texto original, para mostrarlo
    std::vector<std::string> campos;
    int nLinea = 0;
};

class ClienteArchivo {
public:
    ClienteArchivo(std::vector<std::string> gts, const std::string& notif)
        : enlace_(ctx_, std::move(gts), 4000, 3), sub_(crearSocket(ctx_, zmq::socket_type::sub)), notif_(notif) {
        sub_.connect(notif);
        base_ = static_cast<long>(std::time(nullptr) % 10000000L) * 1000000L;
    }

    void suscribir(int idCliente) {
        sub_.set(zmq::sockopt::subscribe, "c" + std::to_string(idCliente) + "|");
    }

    // Devuelve false si la línea no se entiende.
    bool ejecutar(const Operacion& op, bool todo) {
        const auto& c = op.campos;
        const std::string& cmd = c[0];
        Mensaje m;
        m.idMensaje = base_ + (++seq_);
        m.tsEnvio = relojPared();
        m.endpointCliente = "sub:" + notif_;
        try {
            if (cmd == "CONSULTA" && c.size() == 2) {
                m.tipo = TipoOperacion::CONSULTA;
                m.idCliente = 1;
                m.mes = std::stoi(c[1]);
            } else if (cmd == "RESERVA" && c.size() == 5) {
                m.tipo = TipoOperacion::RESERVA;
                m.idCliente = std::stoi(c[1]);
                m.idEvento = std::stoi(c[2]);
                m.idOcurrencia = std::stoi(c[3]);
                m.numAsientos = std::stoi(c[4]);
            } else if (cmd == "MODIFICAR_ASIENTOS" && c.size() == 7) {
                m.tipo = TipoOperacion::MODIFICACION;
                m.idCliente = std::stoi(c[1]);
                m.idReserva = reserva(c[2]);
                m.idEvento = std::stoi(c[3]);
                m.idOcurrencia = std::stoi(c[4]);
                m.numAsientos = std::stoi(c[5]);
                m.numAsientosNuevo = std::stoi(c[6]);
            } else if (cmd == "MODIFICAR_FECHA" && c.size() == 7) {
                m.tipo = TipoOperacion::MODIFICACION;
                m.idCliente = std::stoi(c[1]);
                m.idReserva = reserva(c[2]);
                m.idEvento = std::stoi(c[3]);
                m.idOcurrencia = std::stoi(c[4]);
                m.numAsientos = std::stoi(c[5]);
                m.fechaNueva = std::stoi(c[6]);
            } else if (cmd == "CANCELAR" && c.size() == 3) {
                m.tipo = TipoOperacion::CANCELACION;
                m.idCliente = std::stoi(c[1]);
                m.idReserva = reserva(c[2]);
            } else {
                return false;
            }
        } catch (const std::exception&) {
            return false;
        }

        Mensaje r;
        int intentos = 0, cambios = 0;
        double t0 = ahoraSeg();
        bool ok = enlace_.pedir(m, r, intentos, cambios);
        double ms = (ahoraSeg() - t0) * 1000.0;
        ++n_;
        if (!ok) {
            std::printf("[%3d] %-60s -> SIN RESPUESTA tras %d intentos\n", n_, op.linea.c_str(), intentos);
            ++sinRespuesta_;
            return true;
        }
        std::string extra;
        if (m.tipo == TipoOperacion::RESERVA && r.resultado == Estado::EXITO) {
            ultima_ = r.idReserva;
            extra = " reserva=" + std::to_string(r.idReserva);
        } else if (m.tipo == TipoOperacion::CANCELACION) {
            extra = " (aceptada; se efectuará de forma asíncrona y llegará una notificación)";
        } else if (m.tipo == TipoOperacion::CONSULTA) {
            extra = " eventos=" + std::to_string(r.eventos.size());
        }
        if (intentos > 1) extra += " [reintentos=" + std::to_string(intentos - 1) + "]";
        std::printf("[%3d] %-60s -> %s%s (%.1f ms)\n", n_, op.linea.c_str(), nombre(r.resultado), extra.c_str(), ms);
        if (m.tipo == TipoOperacion::CONSULTA) imprimirEventos(r.eventos, todo);
        if (r.resultado == Estado::ERROR) ++errores_;
        revisarNotificaciones(0);
        return true;
    }

    // Imprime las notificaciones que lleguen durante 'ms' milisegundos.
    void revisarNotificaciones(int ms) {
        double fin = ahoraSeg() + ms / 1000.0;
        do {
            int resto = std::max(0, static_cast<int>((fin - ahoraSeg()) * 1000));
            if (!hayEntrada(sub_, ms == 0 ? 0 : std::min(resto, 200))) continue;
            std::vector<std::string> t;
            Mensaje n;
            if (recibirMultipart(sub_, t) && t.size() >= 2 && deserializar(t.back(), n)) {
                ++notificaciones_;
                std::printf("      NOTIFICACION cliente=%d: cancelación de la reserva %d -> %s\n", n.idCliente,
                            n.idReserva, nombre(n.resultado));
            }
        } while (ahoraSeg() < fin);
    }

    void resumen() const {
        std::printf("\nResumen: %d operaciones, %d sin respuesta, %d con ERROR, %d notificaciones recibidas\n", n_,
                    sinRespuesta_, errores_, notificaciones_);
    }
    int fallos() const { return sinRespuesta_ + errores_; }

private:
    int reserva(const std::string& s) const {
        if (s == "$ULTIMA") {
            if (ultima_ == 0) throw std::runtime_error("$ULTIMA sin reserva previa");
            return ultima_;
        }
        return std::stoi(s);
    }

    static void imprimirEventos(const std::vector<Evento>& ev, bool todo) {
        int mostrados = 0;
        for (const Evento& e : ev) {
            if (!todo && mostrados >= 5) break;
            std::printf("      E%d/occ%d fecha=%d libres=%d ocupados=%d\n", e.idEvento, e.idOcurrencia, e.fecha,
                        e.asientosLibres, e.asientosOcupados);
            ++mostrados;
        }
        if (!todo && ev.size() > 5) std::printf("      ... (%zu eventos más; use --todo para verlos)\n", ev.size() - 5);
    }

    zmq::context_t ctx_{1};
    EnlaceGT enlace_;
    zmq::socket_t sub_;
    std::string notif_;
    long base_ = 0, seq_ = 0;
    int ultima_ = 0;
    int n_ = 0, sinRespuesta_ = 0, errores_ = 0, notificaciones_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    std::string archivo = argc > 1 && argv[1][0] != '-' ? argv[1] : "";
    if (archivo.empty()) {
        std::fprintf(stderr, "Uso: %s operaciones.txt [--gt ...] [--notif ...] [--todo] [--esperar-notif ms]\n", argv[0]);
        return 2;
    }
    std::ifstream in(archivo);
    if (!in) {
        std::fprintf(stderr, "No se puede abrir %s\n", archivo.c_str());
        return 2;
    }
    std::vector<Operacion> ops;
    std::set<int> clientes;
    std::string linea;
    int nl = 0;
    while (std::getline(in, linea)) {
        ++nl;
        size_t h = linea.find('#');
        if (h != std::string::npos) linea.erase(h);
        std::replace(linea.begin(), linea.end(), '\t', ' ');
        std::vector<std::string> campos = partir(linea, ' ');
        if (campos.empty()) continue;
        std::string limpia;
        for (size_t i = 0; i < campos.size(); ++i) limpia += (i ? " " : "") + campos[i];
        ops.push_back({limpia, campos, nl});
        if (campos.size() >= 2 && campos[0] != "CONSULTA" && campos[0] != "PAUSA") {
            try {
                clientes.insert(std::stoi(campos[1]));
            } catch (...) {
            }
        }
    }

    instalarSenales();
    ClienteArchivo cli(partir(argStr(argc, argv, "--gt", "tcp://localhost:5555"), ','),
                       argStr(argc, argv, "--notif", "tcp://localhost:5558"));
    for (int c : clientes) cli.suscribir(c);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));  // que el SUB se conecte

    const bool todo = argFlag(argc, argv, "--todo");
    const int pausa = argInt(argc, argv, "--pausa-ms", 0);
    int malas = 0;
    for (const Operacion& op : ops) {
        if (g_parar) break;
        if (op.campos[0] == "PAUSA" && op.campos.size() == 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(op.campos[1].c_str())));
            continue;
        }
        if (!cli.ejecutar(op, todo)) {
            std::printf("      línea %d no válida, se omite: %s\n", op.nLinea, op.linea.c_str());
            ++malas;
        }
        if (pausa > 0) std::this_thread::sleep_for(std::chrono::milliseconds(pausa));
    }
    cli.revisarNotificaciones(argInt(argc, argv, "--esperar-notif", 3000));
    cli.resumen();
    return cli.fallos() == 0 && malas == 0 ? 0 : 1;
}
