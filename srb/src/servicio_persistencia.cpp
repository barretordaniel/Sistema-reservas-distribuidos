// servicio_persistencia.cpp  (capa de almacenamiento)
// Único componente que habla con PostgreSQL. Atiende por un socket ROUTER las solicitudes
// del RMC y del Servicio de Cancelaciones y las reparte (DEALER interno) entre N hilos; cada
// hilo tiene su propio socket REP y su propia conexión a PostgreSQL, así las operaciones
// sobre eventos distintos se ejecutan en paralelo.
//
// Consistencia:
//  - Reserva/modificación/cancelación son una sola transacción SQL con UPDATE condicionado
//    (asientos_libres >= n) y bloqueos de fila en orden fijo: no hay sobreventa ni interbloqueos.
//  - Idempotencia: la tabla "transaccion" guarda el resultado de cada (idCliente, idMensaje).
//    Si un cliente repite una solicitud (reintento tras un fallo), se devuelve el resultado
//    guardado y la operación NO se vuelve a ejecutar.
//
// Uso:
//   ./persistencia [--bind tcp://*:5559] [--db "dbname=srb"] [--hilos 4] [--reset]
//                  [--mes 202611] [--eventos 50] [--dobles 20] [--asientos 100]
//                  [--volcar-cada SEGUNDOS] [--silencioso]
//   kill -USR1 <pid>   imprime el estado completo de la BD
//   kill -USR2 <pid>   restablece la BD al estado inicial (antes de cada corrida de pruebas)
#include <pqxx/pqxx>

#include <memory>
#include <thread>

#include "common.hpp"

using namespace srb;

struct CargaInicial {
    int eventos = 50;
    int dobles = 20;
    int asientos = 100;
    int mes = 202611;
};

// Acceso a la BD: cada hilo tiene el suyo (una conexión no se comparte entre hilos).
class AccesoBD {
public:
    explicit AccesoBD(std::string cadena) : cadena_(std::move(cadena)) { conectar(); }

    // ---- esquema y carga inicial ----
    bool existeEsquema() {
        pqxx::work tx(*conn_);
        pqxx::result r = tx.exec(
            "SELECT to_regclass('evento') IS NOT NULL AND to_regclass('reserva') IS NOT NULL"
            " AND to_regclass('transaccion') IS NOT NULL");
        tx.commit();
        return r[0][0].as<bool>();
    }

    void crearEsquemaYCargar(const CargaInicial& c) {
        pqxx::work tx(*conn_);
        tx.exec("DROP TABLE IF EXISTS transaccion");
        tx.exec("DROP TABLE IF EXISTS reserva");
        tx.exec("DROP TABLE IF EXISTS evento");
        tx.exec(
            "CREATE TABLE evento ("
            " id_evento INT NOT NULL,"
            " id_ocurrencia INT NOT NULL,"
            " num_ocurrencias INT NOT NULL,"
            " fecha INT NOT NULL,"  // AAAAMMDD
            " asientos_libres INT NOT NULL CHECK (asientos_libres >= 0),"
            " asientos_ocupados INT NOT NULL CHECK (asientos_ocupados >= 0),"
            " PRIMARY KEY (id_evento, id_ocurrencia))");
        tx.exec(
            "CREATE TABLE reserva ("
            " id_reserva SERIAL PRIMARY KEY,"
            " id_cliente INT NOT NULL,"
            " id_evento INT NOT NULL,"
            " id_ocurrencia INT NOT NULL,"
            " num_asientos INT NOT NULL CHECK (num_asientos > 0),"
            " activa BOOLEAN NOT NULL DEFAULT TRUE,"
            " FOREIGN KEY (id_evento, id_ocurrencia) REFERENCES evento (id_evento, id_ocurrencia))");
        // Resultado de cada solicitud ya procesada: permite reconocer los reintentos.
        tx.exec(
            "CREATE TABLE transaccion ("
            " id_cliente INT NOT NULL,"
            " id_mensaje BIGINT NOT NULL,"
            " tipo INT NOT NULL,"
            " resultado INT NOT NULL DEFAULT 2,"
            " id_reserva INT NOT NULL DEFAULT 0,"
            " id_evento INT NOT NULL DEFAULT 0,"
            " id_ocurrencia INT NOT NULL DEFAULT 0,"
            " num_asientos INT NOT NULL DEFAULT 0,"
            " PRIMARY KEY (id_cliente, id_mensaje))");
        tx.exec("CREATE INDEX evento_fecha_idx ON evento (fecha)");
        cargarEventos(tx, c);
        tx.commit();
        logLinea("PERSISTENCIA", "BD creada y cargada: " + std::to_string(c.eventos) +
                                     " eventos (" + std::to_string(c.dobles) +
                                     " con dos ocurrencias), " + std::to_string(c.asientos) +
                                     " asientos por ocurrencia, mes " + std::to_string(c.mes));
    }

    // Deja la BD como al inicio sin borrar las tablas (SIGUSR2).
    void reiniciarDatos(const CargaInicial& c) {
        pqxx::work tx(*conn_);
        tx.exec("TRUNCATE transaccion, reserva, evento RESTART IDENTITY");
        cargarEventos(tx, c);
        tx.commit();
        logLinea("PERSISTENCIA", "BD restablecida al estado inicial");
    }

    // ---- operaciones (cada una devuelve la respuesta; 'duplicado' indica un reintento) ----
    Mensaje atender(const Mensaje& m, bool& duplicado) {
        duplicado = false;
        for (int intento = 0; intento < 6; ++intento) {
            try {
                switch (m.tipo) {
                    case TipoOperacion::CONSULTA: return consultar(m);
                    case TipoOperacion::RESERVA: return reservarAsientos(m, duplicado);
                    case TipoOperacion::MODIFICACION: return modificarReserva(m, duplicado);
                    case TipoOperacion::CANCELACION: return liberarAsientos(m, duplicado);
                }
            } catch (const pqxx::transaction_rollback&) {
                // Interbloqueo o fallo de serialización: PostgreSQL ya deshizo la transacción.
                duplicado = false;
                std::this_thread::sleep_for(std::chrono::milliseconds(2 * (intento + 1)));
            } catch (const pqxx::broken_connection& e) {
                logLinea("PERSISTENCIA", std::string("conexión a la BD perdida: ") + e.what());
                try {
                    conectar();
                } catch (...) {
                }
                break;
            } catch (const std::exception& e) {
                logLinea("PERSISTENCIA", std::string("error de BD: ") + e.what());
                break;
            }
        }
        Mensaje r = m;
        r.resultado = Estado::ERROR;
        return r;
    }

    // CONSULTA: lista de eventos de un mes, o disponibilidad de una ocurrencia.
    Mensaje consultar(const Mensaje& m) {
        Mensaje r = m;
        r.resultado = Estado::EXITO;
        r.eventos.clear();
        pqxx::work tx(*conn_);
        // Consulta de disponibilidad hecha por el RMC como paso previo de una reserva/modificación
        // (mes == 0, con el idMensaje de esa operación). Si la operación YA se ejecutó (el GT la reasignó
        // tras caer un RMC, o el cliente la reintentó), la disponibilidad actual ya descuenta sus propios
        // asientos y haría creer que "no hay cupo". En ese caso se devuelve su resultado guardado, con el
        // tipo original, y el RMC lo reenvía tal cual. Las dos lecturas (tabla transaccion y evento) usan la
        // misma "foto" de la BD (REPEATABLE READ): o la operación ya está confirmada en ambas o en ninguna.
        if (m.mes <= 0 && m.idMensaje > 0 && m.idCliente > 0) {
            tx.exec("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ");
            pqxx::result old = tx.exec_params(
                "SELECT tipo, resultado, id_reserva, id_evento, id_ocurrencia, num_asientos FROM transaccion"
                " WHERE id_cliente = $1 AND id_mensaje = $2",
                m.idCliente, m.idMensaje);
            if (!old.empty()) {
                r.tipo = static_cast<TipoOperacion>(old[0][0].as<int>());
                int e = old[0][1].as<int>();
                r.resultado = e == 0 ? Estado::EXITO : (e == 1 ? Estado::RECHAZO : Estado::ERROR);
                if (old[0][2].as<int>() > 0) r.idReserva = old[0][2].as<int>();
                if (old[0][3].as<int>() > 0) {
                    r.idEvento = old[0][3].as<int>();
                    r.idOcurrencia = old[0][4].as<int>();
                    r.numAsientos = old[0][5].as<int>();
                }
                logOp("PERSISTENCIA", describir(r) + " -> " + nombre(r.resultado) +
                                          " (ya se había ejecutado: se devuelve el resultado guardado)");
                return r;
            }
        }
        pqxx::result res;
        const char* cols =
            "SELECT id_evento, id_ocurrencia, num_ocurrencias, fecha, asientos_libres,"
            " asientos_ocupados FROM evento ";
        if (m.mes > 0) {
            res = tx.exec_params(std::string(cols) + "WHERE fecha / 100 = $1 ORDER BY id_evento, id_ocurrencia",
                                 m.mes);
        } else if (m.fechaNueva > 0) {
            res = tx.exec_params(std::string(cols) + "WHERE id_evento = $1 AND fecha = $2",
                                 m.idEvento, m.fechaNueva);
        } else {
            res = tx.exec_params(std::string(cols) + "WHERE id_evento = $1 AND id_ocurrencia = $2",
                                 m.idEvento, m.idOcurrencia);
        }
        tx.commit();
        for (auto fila : res) {
            Evento e;
            e.idEvento = fila[0].as<int>();
            e.idOcurrencia = fila[1].as<int>();
            e.numOcurrencias = fila[2].as<int>();
            e.fecha = fila[3].as<int>();
            e.asientosLibres = fila[4].as<int>();
            e.asientosOcupados = fila[5].as<int>();
            r.eventos.push_back(e);
        }
        if (m.mes <= 0 && r.eventos.empty()) r.resultado = Estado::RECHAZO;  // no existe
        return r;
    }

    // RESERVA: el UPDATE solo afecta la fila si aún hay asientos suficientes.
    Mensaje reservarAsientos(const Mensaje& m, bool& duplicado) {
        Mensaje r = m;
        if (m.numAsientos < 1) {
            r.resultado = Estado::ERROR;
            return r;
        }
        pqxx::work tx(*conn_);
        if (yaProcesada(tx, m, r)) {
            duplicado = true;
            return r;
        }
        r.resultado = Estado::RECHAZO;  // sin cupo (o el evento no existe)
        pqxx::result u = tx.exec_params(
            "UPDATE evento SET asientos_libres = asientos_libres - $1,"
            " asientos_ocupados = asientos_ocupados + $1"
            " WHERE id_evento = $2 AND id_ocurrencia = $3 AND asientos_libres >= $1",
            m.numAsientos, m.idEvento, m.idOcurrencia);
        if (u.affected_rows() == 1) {
            pqxx::result ins = tx.exec_params(
                "INSERT INTO reserva (id_cliente, id_evento, id_ocurrencia, num_asientos)"
                " VALUES ($1, $2, $3, $4) RETURNING id_reserva",
                m.idCliente, m.idEvento, m.idOcurrencia, m.numAsientos);
            r.idReserva = ins[0][0].as<int>();
            r.resultado = Estado::EXITO;
        }
        guardarResultado(tx, m, r);
        tx.commit();
        return r;
    }

    // MODIFICACION: cambia el número de asientos o pasa la reserva a otra ocurrencia del
    // mismo evento en el mismo mes (conservando los asientos). Si no es posible, no cambia nada.
    // Orden de bloqueo fijo: reserva -> ocurrencias en orden ascendente (evita interbloqueos).
    Mensaje modificarReserva(const Mensaje& m, bool& duplicado) {
        Mensaje r = m;
        pqxx::work tx(*conn_);
        if (yaProcesada(tx, m, r)) {
            duplicado = true;
            return r;
        }
        r.resultado = Estado::RECHAZO;
        pqxx::result res = tx.exec_params(
            "SELECT id_evento, id_ocurrencia, num_asientos FROM reserva"
            " WHERE id_reserva = $1 AND id_cliente = $2 AND activa FOR NO KEY UPDATE",
            m.idReserva, m.idCliente);
        if (!res.empty()) {
            int ev = res[0][0].as<int>(), oc = res[0][1].as<int>(), n = res[0][2].as<int>();
            if (m.fechaNueva > 0) {
                pqxx::result dst = tx.exec_params(
                    "SELECT d.id_ocurrencia FROM evento d JOIN evento o ON o.id_evento = d.id_evento"
                    " WHERE d.id_evento = $1 AND o.id_ocurrencia = $2 AND d.fecha = $3"
                    " AND d.fecha / 100 = o.fecha / 100",
                    ev, oc, m.fechaNueva);
                int oc2 = dst.empty() ? oc : dst[0][0].as<int>();
                if (oc2 != oc) {
                    tx.exec_params(
                        "SELECT 1 FROM evento WHERE id_evento = $1 AND id_ocurrencia IN ($2, $3)"
                        " ORDER BY id_ocurrencia FOR NO KEY UPDATE",
                        ev, oc, oc2);
                    pqxx::result a = tx.exec_params(
                        "UPDATE evento SET asientos_libres = asientos_libres - $1,"
                        " asientos_ocupados = asientos_ocupados + $1"
                        " WHERE id_evento = $2 AND id_ocurrencia = $3 AND asientos_libres >= $1",
                        n, ev, oc2);
                    if (a.affected_rows() == 1) {
                        tx.exec_params(
                            "UPDATE evento SET asientos_libres = asientos_libres + $1,"
                            " asientos_ocupados = asientos_ocupados - $1"
                            " WHERE id_evento = $2 AND id_ocurrencia = $3",
                            n, ev, oc);
                        tx.exec_params("UPDATE reserva SET id_ocurrencia = $1 WHERE id_reserva = $2",
                                       oc2, m.idReserva);
                        r.idEvento = ev;
                        r.idOcurrencia = oc2;
                        r.numAsientos = n;
                        r.resultado = Estado::EXITO;
                    }
                }
            } else if (m.numAsientosNuevo > 0) {
                int delta = m.numAsientosNuevo - n;
                bool ok = true;
                if (delta != 0) {
                    pqxx::result a = tx.exec_params(
                        "UPDATE evento SET asientos_libres = asientos_libres - $1,"
                        " asientos_ocupados = asientos_ocupados + $1"
                        " WHERE id_evento = $2 AND id_ocurrencia = $3 AND asientos_libres >= $1",
                        delta, ev, oc);
                    ok = a.affected_rows() == 1;
                    if (ok)
                        tx.exec_params("UPDATE reserva SET num_asientos = $1 WHERE id_reserva = $2",
                                       m.numAsientosNuevo, m.idReserva);
                }
                if (ok) {
                    r.idEvento = ev;
                    r.idOcurrencia = oc;
                    r.numAsientos = m.numAsientosNuevo;
                    r.resultado = Estado::EXITO;
                }
            }
        }
        guardarResultado(tx, m, r);
        tx.commit();
        return r;
    }

    // CANCELACION: marca la reserva como inactiva y devuelve sus asientos al evento.
    Mensaje liberarAsientos(const Mensaje& m, bool& duplicado) {
        Mensaje r = m;
        pqxx::work tx(*conn_);
        if (yaProcesada(tx, m, r)) {
            duplicado = true;
            return r;
        }
        r.resultado = Estado::RECHAZO;  // no existe, no es del cliente o ya estaba cancelada
        pqxx::result u = tx.exec_params(
            "UPDATE reserva SET activa = FALSE"
            " WHERE id_reserva = $1 AND id_cliente = $2 AND activa"
            " RETURNING id_evento, id_ocurrencia, num_asientos",
            m.idReserva, m.idCliente);
        if (!u.empty()) {
            int ev = u[0][0].as<int>(), oc = u[0][1].as<int>(), n = u[0][2].as<int>();
            tx.exec_params(
                "UPDATE evento SET asientos_libres = asientos_libres + $1,"
                " asientos_ocupados = asientos_ocupados - $1"
                " WHERE id_evento = $2 AND id_ocurrencia = $3",
                n, ev, oc);
            r.idEvento = ev;
            r.idOcurrencia = oc;
            r.numAsientos = n;
            r.resultado = Estado::EXITO;
        }
        guardarResultado(tx, m, r);
        tx.commit();
        return r;
    }

    // Imprime el estado de la BD (resumen o detalle por ocurrencia).
    void volcarEstado(bool detalle) {
        try {
            pqxx::work tx(*conn_);
            pqxx::result ev = tx.exec(
                "SELECT id_evento, id_ocurrencia, num_ocurrencias, fecha, asientos_libres,"
                " asientos_ocupados FROM evento ORDER BY id_evento, id_ocurrencia");
            pqxx::result rs = tx.exec("SELECT COUNT(*) FROM reserva WHERE activa");
            tx.commit();
            long libres = 0, ocupados = 0;
            for (auto f : ev) {
                libres += f[4].as<int>();
                ocupados += f[5].as<int>();
                if (detalle)
                    logLinea("BD", "E" + std::to_string(f[0].as<int>()) + " occ" +
                                       std::to_string(f[1].as<int>()) + "/" +
                                       std::to_string(f[2].as<int>()) +
                                       " fecha=" + std::to_string(f[3].as<int>()) +
                                       " libres=" + std::to_string(f[4].as<int>()) +
                                       " ocupados=" + std::to_string(f[5].as<int>()));
            }
            logLinea("BD", "TOTAL ocurrencias=" + std::to_string(ev.size()) +
                               " libres=" + std::to_string(libres) +
                               " ocupados=" + std::to_string(ocupados) +
                               " reservas_activas=" + std::to_string(rs[0][0].as<long>()));
        } catch (const std::exception& e) {
            logLinea("BD", std::string("no se pudo volcar el estado: ") + e.what());
        }
    }

private:
    void conectar() { conn_.reset(new pqxx::connection(cadena_)); }

    static void cargarEventos(pqxx::work& tx, const CargaInicial& c) {
        for (int e = 1; e <= c.eventos; ++e) {
            int ocurrencias = (e <= c.dobles) ? 2 : 1;
            for (int o = 1; o <= ocurrencias; ++o) {
                // Días 1..14 para la primera ocurrencia y 15..28 para la segunda.
                int dia = (o == 1 ? 1 : 15) + (e % 14);
                int fecha = c.mes * 100 + dia;
                tx.exec_params(
                    "INSERT INTO evento (id_evento, id_ocurrencia, num_ocurrencias, fecha,"
                    " asientos_libres, asientos_ocupados) VALUES ($1, $2, $3, $4, $5, 0)",
                    e, o, ocurrencias, fecha, c.asientos);
            }
        }
    }

    // Idempotencia. Registra la solicitud como la primera instrucción de la transacción; si ya
    // existía (reintento), espera a que termine la original y devuelve su resultado guardado.
    static bool yaProcesada(pqxx::work& tx, const Mensaje& m, Mensaje& r) {
        if (m.idMensaje <= 0 || m.idCliente <= 0) return false;
        pqxx::result ins = tx.exec_params(
            "INSERT INTO transaccion (id_cliente, id_mensaje, tipo) VALUES ($1, $2, $3)"
            " ON CONFLICT DO NOTHING RETURNING 1",
            m.idCliente, m.idMensaje, static_cast<int>(m.tipo));
        if (!ins.empty()) return false;
        pqxx::result old = tx.exec_params(
            "SELECT resultado, id_reserva, id_evento, id_ocurrencia, num_asientos FROM transaccion"
            " WHERE id_cliente = $1 AND id_mensaje = $2",
            m.idCliente, m.idMensaje);
        r = m;
        if (old.empty()) {
            r.resultado = Estado::ERROR;
            return true;
        }
        int e = old[0][0].as<int>();
        r.resultado = e == 0 ? Estado::EXITO : (e == 1 ? Estado::RECHAZO : Estado::ERROR);
        r.idReserva = old[0][1].as<int>() > 0 ? old[0][1].as<int>() : m.idReserva;
        if (old[0][2].as<int>() > 0) {
            r.idEvento = old[0][2].as<int>();
            r.idOcurrencia = old[0][3].as<int>();
            r.numAsientos = old[0][4].as<int>();
        }
        return true;
    }

    static void guardarResultado(pqxx::work& tx, const Mensaje& m, const Mensaje& r) {
        if (m.idMensaje <= 0 || m.idCliente <= 0) return;
        tx.exec_params(
            "UPDATE transaccion SET resultado = $1, id_reserva = $2, id_evento = $3,"
            " id_ocurrencia = $4, num_asientos = $5 WHERE id_cliente = $6 AND id_mensaje = $7",
            static_cast<int>(r.resultado), r.idReserva, r.idEvento, r.idOcurrencia, r.numAsientos,
            m.idCliente, m.idMensaje);
    }

    std::string cadena_;
    std::unique_ptr<pqxx::connection> conn_;
};

class ServicioPersistencia {
public:
    ServicioPersistencia(const std::string& bind, const std::string& cadenaBD, int hilos,
                         const CargaInicial& carga)
        : ctx_(1),
          sockRouter(crearSocket(ctx_, zmq::socket_type::router)),
          sockDealer(crearSocket(ctx_, zmq::socket_type::dealer)),
          cadenaBD_(cadenaBD),
          hilos_(hilos < 1 ? 1 : hilos),
          carga_(carga),
          bd(cadenaBD) {
        sockRouter.bind(bind);
        sockDealer.set(zmq::sockopt::sndhwm, 1);  // no acumular trabajo en un hilo ocupado
        sockDealer.bind("inproc://trabajadores");
    }

    AccesoBD& acceso() { return bd; }

    void iniciar(int volcarCada) {
        std::vector<std::thread> hilos;
        for (int i = 0; i < hilos_; ++i) hilos.emplace_back(&ServicioPersistencia::trabajar, this, i);
        logLinea("PERSISTENCIA", "escuchando con " + std::to_string(hilos_) +
                                     " hilos; esperando solicitudes");
        zmq_pollitem_t items[2] = {{static_cast<void*>(sockRouter), 0, ZMQ_POLLIN, 0},
                                   {static_cast<void*>(sockDealer), 0, ZMQ_POLLIN, 0}};
        double ultimoVolcado = ahoraSeg();
        std::vector<std::string> tramas;
        while (!g_parar) {
            if (g_volcar) {
                g_volcar = 0;
                bd.volcarEstado(true);
            }
            if (g_reiniciar) {
                g_reiniciar = 0;
                try {
                    bd.reiniciarDatos(carga_);
                } catch (const std::exception& e) {
                    logLinea("PERSISTENCIA", std::string("no se pudo restablecer la BD: ") + e.what());
                }
            }
            if (volcarCada > 0 && ahoraSeg() - ultimoVolcado >= volcarCada) {
                ultimoVolcado = ahoraSeg();
                bd.volcarEstado(false);
            }
            if (zmq_poll(items, 2, 200) <= 0) continue;
            // Los mensajes pasan tal cual: [identidad, "", mensaje] hacia los hilos y de vuelta.
            if ((items[0].revents & ZMQ_POLLIN) && recibirMultipart(sockRouter, tramas))
                enviarMultipart(sockDealer, tramas);
            if ((items[1].revents & ZMQ_POLLIN) && recibirMultipart(sockDealer, tramas))
                enviarMultipart(sockRouter, tramas);
        }
        logLinea("PERSISTENCIA", "deteniendo");
        for (auto& h : hilos) h.join();
        bd.volcarEstado(true);
    }

private:
    void trabajar(int idx) {
        try {
            AccesoBD acceso(cadenaBD_);
            zmq::socket_t rep = crearSocket(ctx_, zmq::socket_type::rep);
            rep.set(zmq::sockopt::rcvhwm, 1);
            rep.connect("inproc://trabajadores");
            std::vector<std::string> tramas;
            const std::string tag = "PERSISTENCIA[h" + std::to_string(idx) + "]";
            while (!g_parar) {
                if (!hayEntrada(rep, 200)) continue;
                if (!recibirMultipart(rep, tramas)) continue;
                Mensaje m, r;
                bool dup = false;
                if (tramas.empty() || !deserializar(tramas.back(), m)) {
                    r.resultado = Estado::ERROR;
                    logLinea(tag, "mensaje mal formado -> ERROR");
                } else {
                    r = acceso.atender(m, dup);
                    std::string extra;
                    if (m.tipo == TipoOperacion::RESERVA && r.resultado == Estado::EXITO)
                        extra = " reserva=" + std::to_string(r.idReserva);
                    if (dup) extra += " (duplicado: se devuelve el resultado guardado)";
                    logOp(tag, describir(m) + " -> " + nombre(r.resultado) + extra);
                }
                enviarMultipart(rep, {serializar(r)});  // REP siempre debe responder
            }
        } catch (const std::exception& e) {
            logLinea("PERSISTENCIA", "el hilo " + std::to_string(idx) + " terminó: " + e.what());
        }
    }

    zmq::context_t ctx_;
    zmq::socket_t sockRouter;  // <- RMC y Cancelaciones
    zmq::socket_t sockDealer;  // -> hilos
    std::string cadenaBD_;
    int hilos_;
    CargaInicial carga_;
    AccesoBD bd;  // conexión del hilo principal (carga inicial, volcados, restablecer)
};

int main(int argc, char** argv) {
    instalarSenales();
    g_silencio = argFlag(argc, argv, "--silencioso");
    std::string bind = argStr(argc, argv, "--bind", "tcp://*:5559");
    std::string bd = argStr(argc, argv, "--db", "dbname=srb");
    CargaInicial carga;
    carga.eventos = argInt(argc, argv, "--eventos", 50);
    carga.dobles = argInt(argc, argv, "--dobles", 20);
    carga.asientos = argInt(argc, argv, "--asientos", 100);
    carga.mes = argInt(argc, argv, "--mes", 202611);
    try {
        ServicioPersistencia p(bind, bd, argInt(argc, argv, "--hilos", 4), carga);
        if (argFlag(argc, argv, "--reset") || !p.acceso().existeEsquema())
            p.acceso().crearEsquemaYCargar(carga);
        p.acceso().volcarEstado(true);  // estado inicial
        p.iniciar(argInt(argc, argv, "--volcar-cada", 0));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error fatal: %s\n", e.what());
        return 1;
    }
    return 0;
}
