#include "NtpServer.h"

#ifdef ESP32

#include "logging.h"

NtpServer::NtpServer()
    : _udp(0), _running(false), _drop(false), _time_set(false),
      _base_epoch(0), _base_millis(0), _mux(portMUX_INITIALIZER_UNLOCKED)
{
    memset(&_stat, 0, sizeof(_stat));
}

bool NtpServer::begin(const uint32_t epoch, const uint16_t port)
{
    set_time(epoch);

    if (_running)
        return true;

    _udp = new AsyncUDP();
    if (_udp == 0 || !_udp->listen(port))
    {
        LOG_ERROR("NTP: unable to listen on port " << port);
        delete _udp;
        _udp = 0;
        return false;
    }

    _udp->onPacket([this](AsyncUDPPacket packet) { handle(packet); });

    _running = true;
    LOG_INFO("NTP: listening on port " << port << ", epoch " << epoch);
    return true;
}

void NtpServer::stop()
{
    if (!_running)
        return;

    delete _udp;      // деструктор снимает обработчик и освобождает порт
    _udp = 0;
    _running = false;
    LOG_INFO("NTP: stopped");
}

void NtpServer::set_time(const uint32_t epoch)
{
    portENTER_CRITICAL(&_mux);
    _base_epoch = epoch;
    _base_millis = millis();
    _time_set = true;
    portEXIT_CRITICAL(&_mux);
}

void NtpServer::set_drop(const bool on)
{
    _drop = on;
    LOG_INFO("NTP: drop " << (on ? "on" : "off"));
}

uint32_t NtpServer::now_epoch() const
{
    portENTER_CRITICAL(&_mux);
    const uint32_t base_epoch = _base_epoch;
    const uint32_t base_millis = _base_millis;
    portEXIT_CRITICAL(&_mux);

    return base_epoch + (millis() - base_millis) / 1000UL;
}

uint32_t NtpServer::now_msec() const
{
    portENTER_CRITICAL(&_mux);
    const uint32_t base_millis = _base_millis;
    portEXIT_CRITICAL(&_mux);

    return (millis() - base_millis) % 1000UL;
}

NtpServer::Stat NtpServer::stat() const
{
    portENTER_CRITICAL(&_mux);
    const Stat copy = _stat;
    portEXIT_CRITICAL(&_mux);

    return copy;
}

void NtpServer::handle(AsyncUDPPacket &packet)
{
    portENTER_CRITICAL(&_mux);
    _stat.requests++;
    _stat.last_client = packet.remoteIP();
    portEXIT_CRITICAL(&_mux);

    if (!ntp_is_client_request(packet.data(), packet.length()))
    {
        portENTER_CRITICAL(&_mux);
        _stat.ignored++;
        portEXIT_CRITICAL(&_mux);
        return;
    }

    if (_drop || !_time_set)
    {
        portENTER_CRITICAL(&_mux);
        _stat.dropped++;
        portEXIT_CRITICAL(&_mux);
        return;
    }

    // Время получения и время отправки берём по отдельности: между ними
    // сборка пакета, и для клиента это и есть время обработки на сервере.
    const uint32_t recv_epoch = now_epoch();
    const uint32_t recv_msec = now_msec();

    uint8_t reply[NTP_PACKET_SIZE];
    ntp_build_reply(reply, packet.data(), recv_epoch, recv_msec,
                    now_epoch(), now_msec());

    // Ответ уходит из того же pcb, которым занят порт 123: клиент принимает
    // ответ, только если он пришёл с порта NTP.
    packet.write(reply, NTP_PACKET_SIZE);

    portENTER_CRITICAL(&_mux);
    _stat.replies++;
    _stat.last_epoch = recv_epoch;
    portEXIT_CRITICAL(&_mux);
}

#endif // ESP32
