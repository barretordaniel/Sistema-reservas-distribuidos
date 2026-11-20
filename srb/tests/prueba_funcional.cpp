// prueba_funcional.cpp: prueba de extremo a extremo contra un sistema recién restablecido
// (50 eventos, 20 con dos ocurrencias, 100 asientos, mes 202611). Hace de cliente: envía una
// secuencia fija de operaciones al GT y comprueba cada respuesta y el estado resultante.
// Al final imprime el estado esperado de la BD para contrastarlo con SQL (lo hace el script).
//
// Uso: bin/prueba_funcional [--gt tcp://localhost:5555] [--notif tcp://localhost:5558]
#include <cstdio>
#include <thread>

#include "common.hpp"

using namespace srb;

static int fallos = 0, total = 0;

static void verificar(bool ok, const std::string& caso) {
    ++total;
    if (!ok) ++fallos;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", caso.c_str());
}

class Sesion {
public:
    Sesion(const std::string& gt, const std::string& notif)
        : enlace(ctx, {gt}, 3000, 0), gtEndpoint(gt), sub(crearSocket(ctx, zmq::socket_type::sub)) {
        base = static_cast<long>(std::time(nullptr) % 10000000L) * 1000000L;
        sub.connect(notif);
        for (int c = 1; c <= 2; ++c) sub.set(zmq::sockopt::subscribe, "c" + std::to_string(c) + "|");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));  // que el SUB se conecte
    }

    Mensaje nuevo(TipoOperacion t, int cliente) {
        Mensaje m;
        m.idMensaje = base + (++seq);
        m.tipo = t;
        m.idCliente = cliente;
        m.tsEnvio = relojPared();
        return m;
    }

    // Envía y devuelve la respuesta; si no hay, devuelve ERROR.
    Mensaje enviar(const Mensaje& m, double* ms = nullptr) {
        Mensaje r;
        int intentos, cambios;
        double t0 = ahoraSeg();
        bool ok = enlace.pedir(m, r, intentos, cambios);
        if (ms) *ms = (ahoraSeg() - t0) * 1000.0;
        if (!ok) r.resultado = Estado::ERROR;
        return r;
    }

    Mensaje reservar(int cliente, int ev, int oc, int n) {
        Mensaje m = nuevo(TipoOperacion::RESERVA, cliente);
        m.idEvento = ev;
        m.idOcurrencia = oc;
        m.numAsientos = n;
        return m;
    }
    Mensaje modificarAsientos(int cliente, int idReserva, int ev, int oc, int actual, int nuevoN) {
        Mensaje m = nuevo(TipoOperacion::MODIFICACION, cliente);
        m.idReserva = idReserva;
        m.idEvento = ev;
        m.idOcurrencia = oc;
        m.numAsientos = actual;
        m.numAsientosNuevo = nuevoN;
        return m;
    }
    Mensaje modificarFecha(int cliente, int idReserva, int ev, int oc, int actual, int fecha) {
        Mensaje m = nuevo(TipoOperacion::MODIFICACION, cliente);
        m.idReserva = idReserva;
        m.idEvento = ev;
        m.idOcurrencia = oc;
        m.numAsientos = actual;
        m.fechaNueva = fecha;
        return m;
    }
    Mensaje cancelar(int cliente, int idReserva) {
        Mensaje m = nuevo(TipoOperacion::CANCELACION, cliente);
        m.idReserva = idReserva;
        return m;
    }

    std::vector<Evento> consulta(int mes = 202611) {
        Mensaje m = nuevo(TipoOperacion::CONSULTA, 1);
        m.mes = mes;
        Mensaje r = enviar(m);
        return r.resultado == Estado::EXITO ? r.eventos : std::vector<Evento>();
    }

    // Espera la notificación de una cancelación (tópico del cliente) hasta ms milisegundos.
    bool esperarNotificacion(Mensaje& out, int ms) {
        if (!hayEntrada(sub, ms)) return false;
        std::vector<std::string> t;
        if (!recibirMultipart(sub, t) || t.size() < 2) return false;
        return deserializar(t.back(), out);
    }

    // Envía texto crudo (sin pasar por Mensaje) y devuelve el resultado de la respuesta.
    Estado crudo(const std::string& texto) {
        zmq::socket_t s = crearSocket(ctx, zmq::socket_type::req);
        s.set(zmq::sockopt::rcvtimeo, 2500);
        s.set(zmq::sockopt::sndtimeo, 2500);
        s.connect(gtEndpoint);
        s.send(zmq::buffer(texto), zmq::send_flags::none);
        zmq::message_t r;
        auto ok = s.recv(r, zmq::recv_flags::none);
        if (!ok) return Estado::RECHAZO;  // sin respuesta: se marca distinto de ERROR
        Mensaje m;
        std::string p(static_cast<const char*>(r.data()), r.size());
        return deserializar(p, m) ? m.resultado : Estado::RECHAZO;
    }

    static const Evento* buscar(const std::vector<Evento>& v, int ev, int oc) {
        for (const Evento& e : v)
            if (e.idEvento == ev && e.idOcurrencia == oc) return &e;
        return nullptr;
    }

    zmq::context_t ctx{1};
    EnlaceGT enlace;
    std::string gtEndpoint;
    zmq::socket_t sub;
    long base = 0;
    long seq = 0;
};

static bool libresOcupados(const std::vector<Evento>& v, int ev, int oc, int libres, int ocupados) {
    const Evento* e = Sesion::buscar(v, ev, oc);
    return e && e->asientosLibres == libres && e->asientosOcupados == ocupados;
}

int main(int argc, char** argv) {
    Sesion s(argStr(argc, argv, "--gt", "tcp://localhost:5555"),
             argStr(argc, argv, "--notif", "tcp://localhost:5558"));

    // 1. Consultas
    std::vector<Evento> ev = s.consulta();
    verificar(ev.size() == 70, "consulta del mes: 70 ocurrencias (50 eventos, 20 con dos)");
    int dobles = 0, libresOk = 0;
    for (const Evento& e : ev) {
        dobles += e.numOcurrencias == 2;
        libresOk += (e.asientosLibres == 100 && e.asientosOcupados == 0);
    }
    verificar(dobles == 40 && libresOk == 70, "20 eventos con dos ocurrencias y 100 asientos libres en todas");
    verificar(s.consulta(202612).empty(), "consulta de un mes sin eventos: lista vacía");
    Mensaje mal = s.nuevo(TipoOperacion::CONSULTA, 1);
    mal.mes = 202613;
    verificar(s.enviar(mal).resultado == Estado::ERROR, "consulta con mes inválido (202613) -> ERROR");

    // 2. Reservas
    Mensaje r1 = s.enviar(s.reservar(1, 1, 1, 5));
    verificar(r1.resultado == Estado::EXITO && r1.idReserva >= 1, "reserva de 5 asientos en E1/occ1 -> EXITO");
    ev = s.consulta();
    verificar(libresOcupados(ev, 1, 1, 95, 5), "tras reservar: E1/occ1 con 95 libres y 5 ocupados");
    verificar(s.enviar(s.reservar(1, 1, 1, 96)).resultado == Estado::RECHAZO,
              "reservar más asientos de los libres (96 de 95) -> RECHAZO");
    Mensaje todoE50 = s.reservar(1, 50, 1, 100);
    Mensaje rTodo = s.enviar(todoE50);
    verificar(rTodo.resultado == Estado::EXITO, "reservar los 100 asientos de E50 -> EXITO");
    // Regresión: el reintento (o la reasignación del GT) de la solicitud que agotó el evento tiene que
    // devolver el resultado guardado y NO un RECHAZO por "falta de cupo" (el cupo lo consumió ella misma).
    Mensaje rTodo2 = s.enviar(todoE50);
    verificar(rTodo2.resultado == Estado::EXITO && rTodo2.idReserva == rTodo.idReserva,
              "reintento de la reserva que agotó E50 (mismo idMensaje) -> EXITO y la misma reserva");
    verificar(s.enviar(s.reservar(1, 50, 1, 1)).resultado == Estado::RECHAZO, "reservar en un evento agotado -> RECHAZO");
    verificar(s.enviar(s.reservar(1, 999, 1, 1)).resultado == Estado::RECHAZO, "reservar un evento que no existe -> RECHAZO");
    verificar(s.enviar(s.reservar(1, 2, 1, 0)).resultado == Estado::ERROR, "reservar 0 asientos -> ERROR (validación del GT)");
    verificar(s.enviar(s.reservar(1, 2, 1, 101)).resultado == Estado::ERROR, "reservar 101 asientos -> ERROR (validación del GT)");

    // 3. Idempotencia: el mismo (cliente, mensaje) repetido no reserva dos veces
    Mensaje dup = s.reservar(1, 2, 1, 3);
    Mensaje a = s.enviar(dup), b = s.enviar(dup);
    verificar(a.resultado == Estado::EXITO && b.resultado == Estado::EXITO && a.idReserva == b.idReserva,
              "la misma solicitud repetida devuelve la misma reserva");
    ev = s.consulta();
    verificar(libresOcupados(ev, 2, 1, 97, 3), "la solicitud repetida no ocupó asientos dos veces (E2/occ1: 3 ocupados)");

    // 4. Modificaciones
    Mensaje m1 = s.enviar(s.modificarAsientos(1, r1.idReserva, 1, 1, 5, 8));
    verificar(m1.resultado == Estado::EXITO && m1.numAsientos == 8, "modificar 5 -> 8 asientos -> EXITO");
    ev = s.consulta();
    verificar(libresOcupados(ev, 1, 1, 92, 8), "tras 5 -> 8: E1/occ1 con 92 libres y 8 ocupados");
    verificar(s.enviar(s.modificarAsientos(1, r1.idReserva, 1, 1, 8, 3)).resultado == Estado::EXITO,
              "modificar 8 -> 3 asientos -> EXITO (libera 5)");
    ev = s.consulta();
    verificar(libresOcupados(ev, 1, 1, 97, 3), "tras 8 -> 3: E1/occ1 con 97 libres y 3 ocupados");
    Mensaje r2 = s.enviar(s.reservar(2, 1, 1, 10));
    verificar(r2.resultado == Estado::EXITO, "el cliente 2 reserva 10 asientos en E1/occ1");
    verificar(s.enviar(s.modificarAsientos(1, r1.idReserva, 1, 1, 3, 100)).resultado == Estado::RECHAZO,
              "modificar 3 -> 100 asientos sin cupo -> RECHAZO");
    ev = s.consulta();
    verificar(libresOcupados(ev, 1, 1, 87, 13), "el rechazo no cambió nada (E1/occ1: 87 libres, 13 ocupados)");

    const Evento* o2 = Sesion::buscar(ev, 1, 2);
    int fecha2 = o2 ? o2->fecha : 0;
    Mensaje m2 = s.enviar(s.modificarFecha(1, r1.idReserva, 1, 1, 3, fecha2));
    verificar(m2.resultado == Estado::EXITO && m2.idOcurrencia == 2, "cambiar la reserva a la otra ocurrencia (fecha) -> EXITO");
    ev = s.consulta();
    verificar(libresOcupados(ev, 1, 1, 90, 10) && libresOcupados(ev, 1, 2, 97, 3),
              "tras el cambio de fecha: occ1 recupera 3 asientos y occ2 los ocupa");
    verificar(s.enviar(s.modificarFecha(1, r1.idReserva, 1, 2, 3, fecha2)).resultado == Estado::RECHAZO,
              "cambiar a la ocurrencia en la que ya está -> RECHAZO");
    Mensaje r3 = s.enviar(s.reservar(1, 30, 1, 2));
    verificar(r3.resultado == Estado::EXITO, "reserva de 2 asientos en E30 (una sola ocurrencia)");
    verificar(s.enviar(s.modificarFecha(1, r3.idReserva, 30, 1, 2, 20261128)).resultado == Estado::RECHAZO,
              "cambio de fecha en un evento de una sola ocurrencia -> RECHAZO");
    verificar(s.enviar(s.modificarAsientos(1, r3.idReserva, 30, 1, 2, 4)).resultado == Estado::EXITO,
              "en un evento de una sola ocurrencia solo se admite cambiar asientos -> EXITO");
    verificar(s.enviar(s.modificarAsientos(2, r1.idReserva, 1, 2, 3, 4)).resultado == Estado::RECHAZO,
              "modificar la reserva de otro cliente -> RECHAZO");
    verificar(s.enviar(s.modificarAsientos(1, 999999, 1, 1, 3, 4)).resultado == Estado::RECHAZO,
              "modificar una reserva que no existe -> RECHAZO");
    verificar(s.enviar(s.modificarAsientos(1, r1.idReserva, 1, 2, 3, 0)).resultado == Estado::ERROR,
              "modificar sin fecha ni asientos nuevos -> ERROR (validación del GT)");

    // 5. Cancelación asíncrona: acuse inmediato y notificación después
    double ms = 0;
    Mensaje ack = s.enviar(s.cancelar(1, r1.idReserva), &ms);
    verificar(ack.resultado == Estado::EXITO && ms < 500, "cancelación: acuse de recibo inmediato (< 500 ms)");
    Mensaje nota;
    bool llego = s.esperarNotificacion(nota, 3000);
    verificar(llego && nota.resultado == Estado::EXITO && nota.idReserva == r1.idReserva,
              "llega la notificación de cancelación efectiva al cliente 1");
    ev = s.consulta();
    verificar(libresOcupados(ev, 1, 2, 100, 0), "la cancelación devolvió los 3 asientos a E1/occ2");
    s.enviar(s.cancelar(1, r1.idReserva));
    llego = s.esperarNotificacion(nota, 3000);
    verificar(llego && nota.resultado == Estado::RECHAZO, "cancelar de nuevo la misma reserva -> notificación RECHAZO");
    s.enviar(s.cancelar(1, r2.idReserva));  // la reserva r2 es del cliente 2
    llego = s.esperarNotificacion(nota, 3000);
    verificar(llego && nota.resultado == Estado::RECHAZO, "cancelar la reserva de otro cliente -> notificación RECHAZO");
    verificar(s.enviar(s.cancelar(1, 0)).resultado == Estado::ERROR, "cancelar con idReserva inválido -> ERROR");

    // 6. Mensajes mal formados
    verificar(s.crudo("basura sin formato") == Estado::ERROR, "texto basura -> ERROR");
    verificar(s.crudo(std::string(5000, 'x')) == Estado::ERROR, "mensaje de 5000 bytes -> ERROR");
    verificar(s.crudo("1|9|1||0|0|0|0|0|0|0|0.0|0|") == Estado::ERROR, "tipo de operación inexistente -> ERROR");

    // 7. Estado final
    ev = s.consulta();
    long libres = 0, ocupados = 0;
    for (const Evento& e : ev) {
        libres += e.asientosLibres;
        ocupados += e.asientosOcupados;
    }
    verificar(libres + ocupados == 7000, "libres + ocupados = 7000 (no se perdieron ni crearon asientos)");
    verificar(ocupados == 117, "ocupados = 117 (10 + 3 + 100 + 4)");
    std::printf("\nESPERADO_BD ocupados=117 libres=6883 reservas_activas=4 reservas_total=5\n");
    std::printf("%d pruebas, %d fallos\n", total, fallos);
    return fallos == 0 ? 0 : 1;
}
