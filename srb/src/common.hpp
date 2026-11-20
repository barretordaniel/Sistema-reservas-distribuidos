// common.hpp
// Tipos compartidos (según el diagrama de clases), serialización de mensajes,
// utilidades de ZeroMQ y el enlace con reintentos (Lazy Pirate) de los clientes.
#pragma once

#include <unistd.h>

#include <zmq.hpp>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace srb {

// ---------------------------------------------------------------- Tipos --

enum class TipoOperacion { CONSULTA = 0, RESERVA = 1, MODIFICACION = 2, CANCELACION = 3 };
// SOBRECARGA: el GT descartó la solicitud sin procesarla porque su cubeta (leaky bucket) estaba llena.
enum class Estado { EXITO = 0, RECHAZO = 1, ERROR = 2, SOBRECARGA = 3 };

inline const char* nombre(TipoOperacion t) {
    switch (t) {
        case TipoOperacion::CONSULTA: return "CONSULTA";
        case TipoOperacion::RESERVA: return "RESERVA";
        case TipoOperacion::MODIFICACION: return "MODIFICACION";
        case TipoOperacion::CANCELACION: return "CANCELACION";
    }
    return "?";
}

inline const char* nombre(Estado e) {
    switch (e) {
        case Estado::EXITO: return "EXITO";
        case Estado::RECHAZO: return "RECHAZO";
        case Estado::ERROR: return "ERROR";
        case Estado::SOBRECARGA: return "SOBRECARGA";
    }
    return "?";
}

// Una fila de la tabla evento: una ocurrencia de un evento.
// fecha en formato entero AAAAMMDD (ej. 20261104).
struct Evento {
    int idEvento = 0;
    int idOcurrencia = 0;
    int numOcurrencias = 0;
    int fecha = 0;
    int asientosLibres = 0;
    int asientosOcupados = 0;
};

struct Reserva {
    int idReserva = 0;
    int idCliente = 0;
    int idEvento = 0;
    int idOcurrencia = 0;
    int numAsientos = 0;
    bool activa = true;
};

// Mensaje común a todos los componentes.
//  - mes: formato AAAAMM (ej. 202611).  fechaNueva: formato AAAAMMDD.
//  - (idCliente, idMensaje) identifica la solicitud: un reintento conserva ambos,
//    así Persistencia reconoce los duplicados y no repite la operación.
//  - CONSULTA con mes > 0          -> lista de eventos del mes
//    CONSULTA con mes == 0         -> disponibilidad de (idEvento, idOcurrencia)
//                                     o, si fechaNueva > 0, de la ocurrencia de idEvento en esa fecha
//  - RESERVA                       -> reservar numAsientos de (idEvento, idOcurrencia)
//  - MODIFICACION                  -> sobre idReserva: fechaNueva > 0 cambia de ocurrencia
//                                     conservando los asientos; si no, numAsientosNuevo cambia
//                                     el número de asientos. (idEvento, idOcurrencia, numAsientos
//                                     son los valores actuales que conoce el cliente.)
//  - CANCELACION                   -> liberar los asientos de idReserva
struct Mensaje {
    long idMensaje = 0;  // único por solicitud y por corrida; un reintento lo conserva
    TipoOperacion tipo = TipoOperacion::CONSULTA;
    int idCliente = 0;
    std::string endpointCliente;
    int idEvento = 0;
    int idOcurrencia = 0;
    int mes = 0;
    int numAsientos = 0;
    int idReserva = 0;
    int fechaNueva = 0;
    int numAsientosNuevo = 0;
    std::vector<Evento> eventos;
    double tsEnvio = 0.0;
    Estado resultado = Estado::EXITO;
};

// ---------------------------------------------------------- Serialización --
// Una sola línea de texto, 14 campos separados por '|':
//  idMensaje|tipo|idCliente|endpoint|idEvento|idOcurrencia|mes|numAsientos|
//  idReserva|fechaNueva|numAsientosNuevo|tsEnvio|resultado|eventos
// eventos = lista separada por ';', cada uno "id,ocurrencia,numOcur,fecha,libres,ocupados".

inline std::vector<std::string> dividir(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t ini = 0;
    while (true) {
        size_t p = s.find(sep, ini);
        if (p == std::string::npos) {
            out.push_back(s.substr(ini));
            break;
        }
        out.push_back(s.substr(ini, p - ini));
        ini = p + 1;
    }
    return out;
}

inline std::string serializar(const Mensaje& m) {
    std::string ep = m.endpointCliente;
    for (char& c : ep)
        if (c == '|' || c == ';' || c == ',' || c == '\n') c = '_';
    char ts[48];
    std::snprintf(ts, sizeof ts, "%.6f", m.tsEnvio);
    std::ostringstream o;
    o << m.idMensaje << '|' << static_cast<int>(m.tipo) << '|' << m.idCliente << '|' << ep << '|'
      << m.idEvento << '|' << m.idOcurrencia << '|' << m.mes << '|' << m.numAsientos << '|'
      << m.idReserva << '|' << m.fechaNueva << '|' << m.numAsientosNuevo << '|' << ts << '|'
      << static_cast<int>(m.resultado) << '|';
    for (size_t i = 0; i < m.eventos.size(); ++i) {
        const Evento& e = m.eventos[i];
        if (i) o << ';';
        o << e.idEvento << ',' << e.idOcurrencia << ',' << e.numOcurrencias << ',' << e.fecha << ','
          << e.asientosLibres << ',' << e.asientosOcupados;
    }
    return o.str();
}

// Devuelve false si el texto está mal formado (no lanza excepciones).
inline bool deserializar(const std::string& s, Mensaje& m) {
    try {
        std::vector<std::string> f = dividir(s, '|');
        if (f.size() != 14) return false;
        Mensaje r;
        r.idMensaje = std::stol(f[0]);
        int t = std::stoi(f[1]);
        if (t < 0 || t > 3) return false;
        r.tipo = static_cast<TipoOperacion>(t);
        r.idCliente = std::stoi(f[2]);
        r.endpointCliente = f[3];
        r.idEvento = std::stoi(f[4]);
        r.idOcurrencia = std::stoi(f[5]);
        r.mes = std::stoi(f[6]);
        r.numAsientos = std::stoi(f[7]);
        r.idReserva = std::stoi(f[8]);
        r.fechaNueva = std::stoi(f[9]);
        r.numAsientosNuevo = std::stoi(f[10]);
        r.tsEnvio = std::stod(f[11]);
        int e = std::stoi(f[12]);
        if (e < 0 || e > 3) return false;
        r.resultado = static_cast<Estado>(e);
        if (!f[13].empty()) {
            for (const std::string& item : dividir(f[13], ';')) {
                std::vector<std::string> g = dividir(item, ',');
                if (g.size() != 6) return false;
                Evento ev;
                ev.idEvento = std::stoi(g[0]);
                ev.idOcurrencia = std::stoi(g[1]);
                ev.numOcurrencias = std::stoi(g[2]);
                ev.fecha = std::stoi(g[3]);
                ev.asientosLibres = std::stoi(g[4]);
                ev.asientosOcupados = std::stoi(g[5]);
                r.eventos.push_back(ev);
            }
        }
        m = r;
        return true;
    } catch (...) {
        return false;
    }
}

inline std::string describir(const Mensaje& m) {
    std::ostringstream o;
    o << nombre(m.tipo) << " cli=" << m.idCliente << " msg=" << m.idMensaje;
    switch (m.tipo) {
        case TipoOperacion::CONSULTA:
            if (m.mes > 0) o << " mes=" << m.mes;
            else if (m.fechaNueva > 0) o << " E" << m.idEvento << " fecha=" << m.fechaNueva;
            else o << " E" << m.idEvento << "/occ" << m.idOcurrencia;
            break;
        case TipoOperacion::RESERVA:
            o << " E" << m.idEvento << "/occ" << m.idOcurrencia << " asientos=" << m.numAsientos;
            break;
        case TipoOperacion::CANCELACION: o << " reserva=" << m.idReserva; break;
        case TipoOperacion::MODIFICACION:
            o << " reserva=" << m.idReserva;
            if (m.fechaNueva > 0) o << " fecha->" << m.fechaNueva;
            else o << " asientos->" << m.numAsientosNuevo;
            break;
    }
    return o.str();
}

// ------------------------------------------------------------- Utilidades --

// Reloj monotónico en segundos (para medir tiempos de respuesta).
inline double ahoraSeg() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Reloj de pared en segundos (solo informativo, campo tsEnvio).
inline double relojPared() {
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

// CPU de TODO el nodo (% de la capacidad de todos sus núcleos), medida sobre ventanas de >= 1 s a partir de
// /proc/stat. Con 'simulada' (ruta de un archivo con un número 0..100) devuelve ese valor: sirve para ensayar el
// escalado sin generar carga real. Es seguro llamarla desde varios hilos.
class MedidorCPU {
public:
    explicit MedidorCPU(std::string simulada = "") : sim_(std::move(simulada)) {}
    double porcentaje() {
        if (!sim_.empty()) {
            std::ifstream f(sim_);
            double v = 0;
            if (!(f >> v)) return 0.0;
            return v < 0 ? 0.0 : (v > 100 ? 100.0 : v);
        }
        std::lock_guard<std::mutex> g(mtx_);
        long long busy = 0, total = 0;
        if (!leer(busy, total)) return ultimo_;
        double ahora = ahoraSeg();
        if (t_ == 0.0 || ahora - t_ >= 1.0) {
            if (t_ != 0.0 && total > total_) ultimo_ = 100.0 * (busy - busy_) / static_cast<double>(total - total_);
            busy_ = busy;
            total_ = total;
            t_ = ahora;
        }
        return ultimo_;
    }

private:
    static bool leer(long long& busy, long long& total) {
        std::ifstream f("/proc/stat");
        std::string cpu;
        long long u = 0, n = 0, s = 0, id = 0, io = 0, irq = 0, sirq = 0, st = 0;
        if (!(f >> cpu >> u >> n >> s >> id >> io >> irq >> sirq >> st) || cpu != "cpu") return false;
        busy = u + n + s + irq + sirq + st;
        total = busy + id + io;
        return true;
    }
    std::string sim_;
    std::mutex mtx_;
    long long busy_ = 0, total_ = 0;
    double t_ = 0.0, ultimo_ = 0.0;
};

inline std::string nombreMaquina() {
    char b[128] = {0};
    if (gethostname(b, sizeof b - 1) != 0) return "maquina";
    return b;
}

// Con --silencioso se omiten las líneas por operación (se usa al medir desempeño).
inline bool g_silencio = false;

inline void imprimirLinea(const std::string& componente, const std::string& texto) {
    static std::mutex mtx;
    std::lock_guard<std::mutex> g(mtx);
    using namespace std::chrono;
    auto ahora = system_clock::now();
    std::time_t t = system_clock::to_time_t(ahora);
    int ms = static_cast<int>(duration_cast<milliseconds>(ahora.time_since_epoch()).count() % 1000);
    std::tm tmv;
    localtime_r(&t, &tmv);
    char buf[32];
    std::strftime(buf, sizeof buf, "%H:%M:%S", &tmv);
    std::printf("[%s.%03d] [%s] %s\n", buf, ms, componente.c_str(), texto.c_str());
    std::fflush(stdout);
}

// Línea de log que siempre se imprime (arranque, fallas, estadísticas).
inline void logLinea(const std::string& componente, const std::string& texto) {
    imprimirLinea(componente, texto);
}

// Una línea por operación (para ver qué hace cada proceso en la sustentación).
inline void logOp(const std::string& componente, const std::string& texto) {
    if (!g_silencio) imprimirLinea(componente, texto);
}

// Argumentos: --nombre valor
inline std::string argStr(int argc, char** argv, const std::string& nombre, const std::string& def) {
    for (int i = 1; i < argc - 1; ++i)
        if (nombre == argv[i]) return argv[i + 1];
    return def;
}
inline int argInt(int argc, char** argv, const std::string& nombre, int def) {
    return std::atoi(argStr(argc, argv, nombre, std::to_string(def)).c_str());
}
inline double argDbl(int argc, char** argv, const std::string& nombre, double def) {
    return std::atof(argStr(argc, argv, nombre, std::to_string(def)).c_str());
}
inline bool argFlag(int argc, char** argv, const std::string& nombre) {
    for (int i = 1; i < argc; ++i)
        if (nombre == argv[i]) return true;
    return false;
}

// Señales: SIGINT/SIGTERM detienen el proceso; SIGUSR1 vuelca estado/estadísticas;
// SIGUSR2 (solo Persistencia) restablece la BD al estado inicial.
inline volatile std::sig_atomic_t g_parar = 0;
inline volatile std::sig_atomic_t g_volcar = 0;
inline volatile std::sig_atomic_t g_reiniciar = 0;

inline void alSenal(int s) {
    if (s == SIGUSR1) g_volcar = 1;
    else if (s == SIGUSR2) g_reiniciar = 1;
    else g_parar = 1;
}

inline void instalarSenales() {
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = alSenal;
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGUSR1, &sa, nullptr);
    sigaction(SIGUSR2, &sa, nullptr);
}

// ---------------------------------------------------------- ZeroMQ helpers --

inline zmq::socket_t crearSocket(zmq::context_t& ctx, zmq::socket_type tipo) {
    zmq::socket_t s(ctx, tipo);
    s.set(zmq::sockopt::linger, 0);
    return s;
}

// Identidad fija para sockets DEALER (el GT la usa para reconocer a cada RMC).
inline void fijarIdentidad(zmq::socket_t& s, const std::string& id) {
    zmq_setsockopt(static_cast<void*>(s), ZMQ_ROUTING_ID, id.data(), id.size());
}

// Espera hasta ms milisegundos a que el socket tenga algo para leer.
inline bool hayEntrada(zmq::socket_t& s, int ms) {
    zmq_pollitem_t it = {static_cast<void*>(s), 0, ZMQ_POLLIN, 0};
    return zmq_poll(&it, 1, ms) > 0;
}

// Lee un mensaje completo (todas sus tramas).
inline bool recibirMultipart(zmq::socket_t& s, std::vector<std::string>& tramas) {
    tramas.clear();
    try {
        while (true) {
            zmq::message_t msg;
            auto r = s.recv(msg, zmq::recv_flags::none);
            if (!r) return false;
            tramas.emplace_back(static_cast<const char*>(msg.data()), msg.size());
            if (!msg.more()) break;
        }
        return true;
    } catch (const zmq::error_t&) {
        return false;
    }
}

inline bool enviarMultipart(zmq::socket_t& s, const std::vector<std::string>& tramas,
                            bool sinBloquear = false) {
    try {
        for (size_t i = 0; i < tramas.size(); ++i) {
            auto flag = (i + 1 < tramas.size()) ? zmq::send_flags::sndmore : zmq::send_flags::none;
            if (sinBloquear) flag = flag | zmq::send_flags::dontwait;
            auto r = s.send(zmq::buffer(tramas[i]), flag);
            if (!r) return false;
        }
        return true;
    } catch (const zmq::error_t&) {
        return false;
    }
}

// Socket REQ con timeout. Si no llega respuesta a tiempo, el socket REQ queda
// inutilizable, así que se cierra y se abre uno nuevo.
class EnlaceREQ {
public:
    EnlaceREQ(zmq::context_t& ctx, std::string endpoint, int timeoutMs)
        : ctx_(ctx), endpoint_(std::move(endpoint)), timeoutMs_(timeoutMs) {
        abrir();
    }

    // false = timeout o error de comunicación.
    bool pedir(const Mensaje& req, Mensaje& resp) {
        try {
            std::string datos = serializar(req);
            auto s = sock_.send(zmq::buffer(datos), zmq::send_flags::none);
            if (!s) {
                abrir();
                return false;
            }
            zmq::message_t msg;
            auto r = sock_.recv(msg, zmq::recv_flags::none);
            if (!r) {
                abrir();
                return false;
            }
            std::string payload(static_cast<const char*>(msg.data()), msg.size());
            return deserializar(payload, resp);
        } catch (const zmq::error_t&) {
            abrir();
            return false;
        }
    }

private:
    void abrir() {
        sock_ = crearSocket(ctx_, zmq::socket_type::req);
        sock_.set(zmq::sockopt::rcvtimeo, timeoutMs_);
        sock_.set(zmq::sockopt::sndtimeo, timeoutMs_);
        sock_.connect(endpoint_);
    }

    zmq::context_t& ctx_;
    std::string endpoint_;
    int timeoutMs_;
    zmq::socket_t sock_;
};

// Enlace de un Cliente con el Gestor de Transacciones (patrón Lazy Pirate).
// Recibe una lista de GT (el titular primero, luego los de relevo). Si no hay respuesta
// a tiempo, abre un socket nuevo hacia el siguiente GT y REPITE la misma solicitud con el
// mismo (idCliente, idMensaje): Persistencia reconoce el duplicado y no la ejecuta dos veces.
class EnlaceGT {
public:
    EnlaceGT(zmq::context_t& ctx, std::vector<std::string> endpoints, int timeoutMs, int reintentos)
        : ctx_(ctx), eps_(std::move(endpoints)), timeoutMs_(timeoutMs), reintentos_(reintentos) {
        if (eps_.empty()) eps_.push_back("tcp://localhost:5555");
        abrir();
    }

    // false = se agotaron los intentos. intentos = envíos realizados; cambios = veces que se
    // pasó a otro GT durante esta solicitud.
    bool pedir(const Mensaje& req, Mensaje& resp, int& intentos, int& cambios) {
        intentos = 0;
        cambios = 0;
        std::string datos = serializar(req);
        for (int k = 0; k <= reintentos_ && !g_parar; ++k) {
            ++intentos;
            bool ok = false;
            try {
                auto s = sock_.send(zmq::buffer(datos), zmq::send_flags::none);
                if (s) {
                    zmq::message_t msg;
                    auto r = sock_.recv(msg, zmq::recv_flags::none);
                    if (r) {
                        std::string payload(static_cast<const char*>(msg.data()), msg.size());
                        if (deserializar(payload, resp)) ok = true;
                    }
                }
            } catch (const zmq::error_t&) {
            }
            if (ok) return true;
            if (eps_.size() > 1) {
                actual_ = (actual_ + 1) % eps_.size();
                ++cambios;
            }
            abrir();  // un REQ que no recibió respuesta no se puede reutilizar
        }
        return false;
    }

    const std::string& actual() const { return eps_[actual_]; }

private:
    void abrir() {
        sock_ = crearSocket(ctx_, zmq::socket_type::req);
        sock_.set(zmq::sockopt::rcvtimeo, timeoutMs_);
        sock_.set(zmq::sockopt::sndtimeo, timeoutMs_);
        sock_.connect(eps_[actual_]);
    }

    zmq::context_t& ctx_;
    std::vector<std::string> eps_;
    size_t actual_ = 0;
    int timeoutMs_;
    int reintentos_;
    zmq::socket_t sock_;
};

// Comandos del protocolo GT <-> RMC (Paranoid Pirate): tramas [cmd], [cmd, corr, mensaje].
inline const char* CMD_READY = "READY";
inline const char* CMD_HB = "HB";
inline const char* CMD_REQ = "REQ";
inline const char* CMD_REP = "REP";

}  // namespace srb
