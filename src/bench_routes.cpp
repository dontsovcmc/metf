#include "bench_routes.h"

#include <Wire.h>

#include "http_util.h"
#include "logging.h"
#include "utils.h"

using http::Error;
using http::param_any;
using http::send_400;
using http::send_500;

namespace {

const char *const PARAM_PIN = "pin";
const char *const PARAM_VALUE = "value";
const char *const PARAM_DURATION_MS = "duration_ms";
const char *const PARAM_MODE = "mode";
const char *const PARAM_ACTION = "action";
const char *const PARAM_ACK = "ack";
const char *const PARAM_SDA_PIN = "sda_pin";
const char *const PARAM_SCL_PIN = "scl_pin";
const char *const PARAM_ADDRESS = "address";
const char *const PARAM_HEXSTRING = "hexstring";
const char *const PARAM_RESPONSE = "response";
const char *const PARAM_BAUDRATE = "baudrate";
#ifdef ESP32
const char *const PARAM_EPOCH = "epoch";
#endif

constexpr uint32_t kDefaultBaud = 115200;

constexpr uint32_t kAllowedBauds[] = {300,   1200,  2400,   4800,   9600,   19200,  38400,
                                      57600, 74880, 115200, 230400, 250000, 460800, 921600};

bool is_allowed_baud(uint32_t b) {
    for (auto v : kAllowedBauds)
        if (v == b) return true;
    return false;
}

// Значение поля формы; вызывать после проверки hasParam(name, true)
String form(AsyncWebServerRequest *request, const char *name) {
    return request->getParam(name, true)->value();
}

} // namespace

BenchRoutes::BenchRoutes(HardwareSerial &dut) : dut_(dut), baud_(kDefaultBaud) {}

void BenchRoutes::watch_overruns() {
#ifdef ESP32
    // Потерянное драйвером иначе невидимо: dropped считает только вытесненное
    // из кольца, а до кольца байты уже не дошли. Без этого счётчика «в логе
    // дыры нет» - вера, а не утверждение
    dut_.onReceiveError([this](hardwareSerial_error_t err) {
        if (err == UART_BUFFER_FULL_ERROR || err == UART_FIFO_OVF_ERROR) {
            overruns_.fetch_add(1, std::memory_order_relaxed);
        }
    });
#endif
}

void BenchRoutes::begin() {
    // Запас до того, как драйвер начнёт терять байты: см. kRxBufferBytes
    dut_.setRxBufferSize(kRxBufferBytes);
    dut_.begin(baud_);
    watch_overruns();
}

void BenchRoutes::loop() {
    while (dut_.available() > 0) {
        asb_.pushChar(static_cast<char>(dut_.read()));
    }
}

void BenchRoutes::attach(AsyncWebServer &server) {
    server.on("/ping", HTTP_GET, [](AsyncWebServerRequest *request) {
        LOG_INFO("GET /ping");
        http::reply(request, 200, "text/plain", "pong");
    });

    server.on("/pinMode", HTTP_POST, [this](AsyncWebServerRequest *r) { on_pin_mode(r); });
    server.on("/digitalRead", HTTP_GET, [this](AsyncWebServerRequest *r) { on_digital_read(r); });
    server.on("/digitalWrite", HTTP_POST,
              [this](AsyncWebServerRequest *r) { on_digital_write(r); });
    server.on("/pulse", HTTP_POST, [this](AsyncWebServerRequest *r) { on_pulse(r); });
    server.on("/i2c", HTTP_POST, [this](AsyncWebServerRequest *r) { on_i2c(r); });
    server.on("/serial", HTTP_POST, [this](AsyncWebServerRequest *r) { on_serial(r); });

    // Регистрируется ДО /read: обработчик подходит и по префиксу
    // (`url.startsWith(_uri + "/")` в AsyncCallbackWebHandler::canHandle),
    // поэтому /read, объявленный первым, перехватил бы и /read/stat.
    server.on("/read/stat", HTTP_GET, [this](AsyncWebServerRequest *r) { on_read_stat(r); });
    server.on("/read", HTTP_GET, [this](AsyncWebServerRequest *r) { on_read(r); });

#ifdef ESP32
    // /ntp/stat раньше /ntp - по той же причине, что /read/stat
    server.on("/ntp/stat", HTTP_GET, [this](AsyncWebServerRequest *r) { on_ntp_stat(r); });
    server.on("/ntp", HTTP_POST, [this](AsyncWebServerRequest *r) { on_ntp(r); });
#endif

    server.on("/version", HTTP_GET, [](AsyncWebServerRequest *request) {
        http::reply(request, 200, "text/plain", METF_VERSION);
    });
}

// ---------------------------------------------------------------- GPIO

// POST /pinMode  form: pin=<n>&mode=<INPUT, OUTPUT, INPUT_PULLUP>
void BenchRoutes::on_pin_mode(AsyncWebServerRequest *request) {
    if (!request->hasParam(PARAM_PIN, true)) {
        send_400(request, Error::NoFormParam, PARAM_PIN);
        return;
    }
    if (!request->hasParam(PARAM_MODE, true)) {
        send_400(request, Error::NoFormParam, PARAM_MODE);
        return;
    }

    const auto pin = static_cast<uint8_t>(form(request, PARAM_PIN).toInt());
    const auto mode = static_cast<uint8_t>(form(request, PARAM_MODE).toInt());

    pinMode(pin, mode);
    http::reply(request, 200, "text/plain", "OK");
}

// GET /digitalRead?pin=<n>
void BenchRoutes::on_digital_read(AsyncWebServerRequest *request) {
    if (!request->hasParam(PARAM_PIN)) {
        send_400(request, Error::NoGetParam, PARAM_PIN);
        return;
    }

    const auto pin = static_cast<uint8_t>(request->getParam(PARAM_PIN)->value().toInt());
    http::reply(request, 200, "text/plain", digitalRead(pin) == HIGH ? "1" : "0");
}

// POST /digitalWrite  form: pin=<n>&value=<HIGH, LOW>
void BenchRoutes::on_digital_write(AsyncWebServerRequest *request) {
    if (!request->hasParam(PARAM_PIN, true)) {
        send_400(request, Error::NoFormParam, PARAM_PIN);
        return;
    }
    if (!request->hasParam(PARAM_VALUE, true)) {
        send_400(request, Error::NoFormParam, PARAM_VALUE);
        return;
    }

    const auto pin = static_cast<uint8_t>(form(request, PARAM_PIN).toInt());
    const auto value = static_cast<uint8_t>(form(request, PARAM_VALUE).toInt());

    digitalWrite(pin, value);
    http::reply(request, 200, "text/plain", "OK");
}

// ---------------------------------------------------------------- импульс

void BenchRoutes::pulse_end(int slot) {
    pinMode(pulse_[slot].pin, INPUT); // отпускаем линию в high-Z
    pulse_[slot].busy = false;
}

int BenchRoutes::pulse_slot_of(uint8_t pin) const {
    for (int i = 0; i < kPulseSlots; i++)
        if (pulse_[i].busy && pulse_[i].pin == pin) return i;
    return -1;
}

int BenchRoutes::pulse_slot_free() const {
    for (int i = 0; i < kPulseSlots; i++)
        if (!pulse_[i].busy) return i;
    return -1;
}

// POST /pulse  form: pin=<n>&duration_ms=<ms>&value=<0|1>
void BenchRoutes::on_pulse(AsyncWebServerRequest *request) {
    if (!request->hasParam(PARAM_PIN, true)) {
        send_400(request, Error::NoFormParam, PARAM_PIN);
        return;
    }
    if (!request->hasParam(PARAM_DURATION_MS, true)) {
        send_400(request, Error::NoFormParam, PARAM_DURATION_MS);
        return;
    }
    if (!request->hasParam(PARAM_VALUE, true)) {
        send_400(request, Error::NoFormParam, PARAM_VALUE);
        return;
    }

    const auto pin = static_cast<uint8_t>(form(request, PARAM_PIN).toInt());
    const auto value = static_cast<uint8_t>(form(request, PARAM_VALUE).toInt());
    const auto ms = static_cast<uint32_t>(form(request, PARAM_DURATION_MS).toInt());

    // Отказ - про вывод, а не про плату: по соседнему выводу импульс идти
    // может и должен
    if (pulse_slot_of(pin) >= 0) {
        http::reply(request, 409, "text/plain", "pulse in progress");
        return;
    }

    const int slot = pulse_slot_free();
    if (slot < 0) {
        http::reply(request, 503, "text/plain", "no free pulse timer");
        return;
    }

    pulse_[slot].pin = pin;
    pulse_[slot].busy = true;
    pinMode(pin, OUTPUT);
    digitalWrite(pin, value);
#ifdef ESP8266
    // не SYS-контекст, а loop()
    pulse_[slot].timer.once_ms_scheduled(ms, [this, slot]() { pulse_end(slot); });
#else
    // ESP32: таймер ядра зовёт из задачи esp_timer
    pulse_[slot].timer.once_ms(ms, [this, slot]() { pulse_end(slot); });
#endif

    // Отвечаем сразу: импульс принят, идёт, длится столько-то. Ждать конца
    // клиент обязан по своим часам - плата про этот момент больше не пишет.
    //
    // Ответ отложенным быть не может. Отложенный уходил chunked-ответом,
    // который до конца импульса отдавал RESPONSE_TRY_AGAIN, а наполнитель
    // сервер зовёт заново не в момент готовности, а на следующем опросе
    // AsyncTCP - раз в ~500 мс. Измерено на плате, импульс 20 мс, 20
    // замеров: ответ приходил на 240-336 мс позже отпущенной линии. Стенд
    // отсчитывает от ответа паузу между импульсами, и эта добавка съела
    // запас проверки слипания: два замыкания через 0,3 с attiny обязан
    // слить в один импульс, пока не прошло 750 мс, а выходило 636 мс.
    http::reply(request, 202, "text/plain", String(ms));
}

// ---------------------------------------------------------------- I2C

// POST /i2c  form: action=<begin|setClock|setClockStretchLimit|ask|flush>&...
void BenchRoutes::on_i2c(AsyncWebServerRequest *request) {
    if (!request->hasParam(PARAM_ACTION, true)) {
        send_400(request, Error::NoFormParam, PARAM_ACTION);
        return;
    }

    LOG_INFO("POST /i2c");
    for (size_t i = 0; i < request->params(); i++) {
        const AsyncWebParameter *param = request->getParam(i);
        if (param) {
            LOG_INFO("  " << param->name() << "=" << param->value());
        }
    }

    const String action = form(request, PARAM_ACTION);

    if (action == "begin") {
        uint8_t sda_pin = SDA;
        uint8_t scl_pin = SCL;
        if (request->hasParam(PARAM_SDA_PIN, true) && request->hasParam(PARAM_SCL_PIN, true)) {
            sda_pin = static_cast<uint8_t>(form(request, PARAM_SDA_PIN).toInt());
            scl_pin = static_cast<uint8_t>(form(request, PARAM_SCL_PIN).toInt());
        }

        LOG_INFO("Wire begin SDA=" << sda_pin << " SCL=" << scl_pin);
        Wire.begin(sda_pin, scl_pin);

    } else if (action == "setClock") {
        if (!request->hasParam(PARAM_VALUE, true)) {
            send_400(request, Error::NoFormParam, PARAM_VALUE);
            return;
        }

        const auto clock = static_cast<uint32_t>(form(request, PARAM_VALUE).toInt());
        LOG_INFO("Wire.setClock(" << clock << ")");
        Wire.setClock(clock);

    } else if (action == "setClockStretchLimit") {
        if (!request->hasParam(PARAM_VALUE, true)) {
            send_400(request, Error::NoFormParam, PARAM_VALUE);
            return;
        }

        const auto stretch = static_cast<uint32_t>(form(request, PARAM_VALUE).toInt());
#ifdef ESP8266
        // ESP8266: setClockStretchLimit takes microseconds
        LOG_INFO("Wire.setClockStretchLimit(" << stretch << " us)");
        Wire.setClockStretchLimit(stretch);
#elif defined(ESP32)
        // ESP32/ESP32-C6: setTimeOut takes milliseconds, minimum 1000 ms
        uint32_t timeout_ms = stretch / 1000;
        if (timeout_ms < 1000) timeout_ms = 1000;
        LOG_INFO("Wire.setTimeOut(" << timeout_ms << " ms) [converted from " << stretch << " us]");
        Wire.setTimeOut(timeout_ms);
#endif

    } else if (action == "ask") {
        if (!request->hasParam(PARAM_ADDRESS, true)) {
            send_400(request, Error::NoFormParam, PARAM_ADDRESS);
            return;
        }
        if (!request->hasParam(PARAM_HEXSTRING, true)) {
            send_400(request, Error::NoFormParam, PARAM_HEXSTRING);
            return;
        }
        if (!request->hasParam(PARAM_RESPONSE, true)) {
            send_400(request, Error::NoFormParam, PARAM_RESPONSE);
            return;
        }

        const auto response_len = static_cast<uint8_t>(form(request, PARAM_RESPONSE).toInt());
        const auto address = static_cast<uint8_t>(form(request, PARAM_ADDRESS).toInt());
        String hexstring = form(request, PARAM_HEXSTRING);
        if (hexstring.isEmpty()) { // массив нулевой длины - неопределённое поведение
            send_400(request, Error::IncorrectValue, PARAM_HEXSTRING);
            return;
        }

        uint8_t arr[hexstring.length()]; // NOLINT: VLA, как было; длина ограничена запросом
        const size_t len = hexText2AsciiArray(hexstring, arr, hexstring.length());
        if (len == 0) {
            send_400(request, Error::IncorrectValue, PARAM_HEXSTRING);
            return;
        }

        Wire.beginTransmission(address);
        for (size_t i = 0; i < len; i++) {
            LOG_DEBUG("i2c > " << String(arr[i], 16));
            if (Wire.write(arr[i]) != 1) {
                Wire.endTransmission();
                send_500(request, "i2c write error");
                return;
            }
        }

        // 0 успех, 1 не влезло в буфер, 2 NACK на адресе, 3 NACK на данных,
        // 4 прочее (https://www.arduino.cc/en/Reference/WireEndTransmission)
        const int err = Wire.endTransmission();
        if (err != 0) {
            send_500(request, "i2c end transmission error " + String(err));
            return;
        }

        LOG_INFO("Wire send: " << hexstring << " " << len << " bytes to device " << address);
        delay(1); // Дадим время подумать

        hexstring.clear();
        hexstring.reserve(response_len * 2 + 1);

        for (uint8_t i = 0; i < response_len; i++) {
            if (Wire.requestFrom(address, static_cast<uint8_t>(1)) != 1) {
                send_500(request, "i2c read timeout. Received: " + hexstring);
                return;
            }
            const auto b = static_cast<uint8_t>(Wire.read());
            LOG_DEBUG("i2c < " << String(b, 16));

            hexstring += intToHexChar(b >> 4);
            hexstring += intToHexChar(b & 0x0F);
        }

        LOG_INFO("Received: " << hexstring);
        http::reply(request, 200, "text/plain", hexstring);
        return;

    } else if (action == "flush") {
        Wire.flush();

    } else {
        send_400(request, Error::IncorrectValue, PARAM_ACTION);
        return;
    }
    http::reply(request, 200, "text/plain", "OK");
}

// ---------------------------------------------------------------- UART испытуемого

// POST /serial  baudrate=<baud>&flush=<0|1>, в теле или в строке запроса
void BenchRoutes::on_serial(AsyncWebServerRequest *request) {
    String out;
    uint32_t nb = kDefaultBaud;
    const AsyncWebParameter *baud = param_any(request, PARAM_BAUDRATE);
    if (baud) {
        nb = static_cast<uint32_t>(baud->value().toInt());
    }

    if (nb == 0 || !is_allowed_baud(nb)) {
        http::reply(request, 400, "text/plain; charset=utf-8", "Invalid speed");
        return;
    }

    if (nb != baud_) {
        // Короткая критическая секция: останавливаем приём и переключаем UART
        LOCK();
        dut_.end();
        dut_.setRxBufferSize(kRxBufferBytes);
        dut_.begin(nb);
        baud_ = nb;
        UNLOCK();

        // Только вне критической секции: обработчик создаёт задачу событий
        watch_overruns();

        out = "Set " + String(nb) + " baudrate";
    } else {
        out = "Baudrate is " + String(nb);
    }

    const AsyncWebParameter *flush = param_any(request, "flush");
    if (flush && flush->value() == "1") {
        asb_.flush();
        out += ", flush buffer";
    }

    http::reply(request, 200, "text/plain; charset=utf-8", out);
}

// GET /read/stat - состояние кольца лога: сколько строк лежит, сколько
// вытеснено, сколько раз переполнялся приёмный буфер UART, на какой скорости
// читаем. dropped > 0 или overruns > 0 - в логе дыра, читателю верить нельзя.
// Это разные беды: первая - кольцо не успел разобрать читатель, вторая - loop()
// не успел разобрать драйвер, и строки не дошли даже до кольца.
void BenchRoutes::on_read_stat(AsyncWebServerRequest *request) {
    const String out = "{\"lines\":" + String(static_cast<uint32_t>(asb_.count())) +
                       ",\"seq\":" + String(asb_.seq()) +
                       ",\"dropped\":" + String(asb_.dropped()) +
                       ",\"overruns\":" + String(overruns_.load(std::memory_order_relaxed)) +
                       ",\"baud\":" + String(baud_) +
                       ",\"capacity\":" + String(static_cast<uint32_t>(ASB_MAX_LINES - 1)) +
                       ",\"line_len\":" + String(static_cast<uint32_t>(ASB_MAX_LINE_LEN)) +
                       ",\"bytes\":" +
                       String(static_cast<uint32_t>(ASB_MAX_LINES) * ASB_MAX_LINE_LEN) + "}";
    http::reply(request, 200, "application/json", out);
}

// GET /read[?ack=<номер>] - отдать накопленные строки без добавления разделителей
//
// С `ack` кольцо забывает строки по номер включительно и придерживает
// остальные: не доехавший ответ повторяется тем же запросом и отдаёт то же
// окно. Номер последней строки ответа - в заголовке X-Log-Seq.
//
// Без `ack` - прежнее поведение: отдать и сразу забыть. Оставлено для
// клиентов протокола 11 и ручного curl; строки при потере ответа пропадают.
void BenchRoutes::on_read(AsyncWebServerRequest *request) {
    // Буфер сразу на весь лог: по умолчанию он 1460 байт и на каждой добавке
    // перекладывает всё, что уже накоплено, - на 64 КБ это десятки мегабайт
    // копирования за один /read
    AsyncResponseStream *res = request->beginResponseStream(
        "text/plain; charset=utf-8", (size_t)ASB_MAX_LINES * ASB_MAX_LINE_LEN);

    const AsyncWebParameter *ack = http::param_any(request, PARAM_ACK);
    uint32_t seq;
    if (ack) {
        seq = asb_.read_to(*res, static_cast<uint32_t>(strtoul(ack->value().c_str(), nullptr, 10)));
    } else {
        asb_.drain_to(*res);
        seq = asb_.seq();
    }

    res->addHeader(http::LOG_SEQ_HEADER, String(seq));
    http::stamp(res);
    request->send(res);
}

// ---------------------------------------------------------------- NTP

#ifdef ESP32
// GET /ntp/stat - состояние сервера времени
void BenchRoutes::on_ntp_stat(AsyncWebServerRequest *request) {
    const NtpServer::Stat st = ntp_.stat();
    const String out = String("{\"running\":") + (ntp_.running() ? "true" : "false") +
                       ",\"dropping\":" + (ntp_.dropping() ? "true" : "false") +
                       ",\"epoch\":" + String(ntp_.now_epoch()) +
                       ",\"requests\":" + String(st.requests) +
                       ",\"replies\":" + String(st.replies) + ",\"dropped\":" + String(st.dropped) +
                       ",\"ignored\":" + String(st.ignored) +
                       ",\"last_epoch\":" + String(st.last_epoch) + ",\"last_client\":\"" +
                       st.last_client.toString() + "\"" + "}";
    http::reply(request, 200, "application/json", out);
}

/*
POST /ntp - сервер времени стенда.

action=start&epoch=<unix>  начать отвечать, назначив время
action=time&epoch=<unix>   переставить часы, не трогая слушателя
action=stop                перестать слушать: клиенту придёт отказ порта
action=drop&value=<0|1>    слушать, но молчать: клиент дождётся таймаута
*/
void BenchRoutes::on_ntp(AsyncWebServerRequest *request) {
    if (!request->hasParam(PARAM_ACTION, true)) {
        send_400(request, Error::NoFormParam, PARAM_ACTION);
        return;
    }
    const String action = form(request, PARAM_ACTION);
    LOG_INFO("POST /ntp action=" << action);

    if (action == "start" || action == "time") {
        if (!request->hasParam(PARAM_EPOCH, true)) {
            send_400(request, Error::NoFormParam, PARAM_EPOCH);
            return;
        }
        const auto epoch =
            static_cast<uint32_t>(strtoul(form(request, PARAM_EPOCH).c_str(), nullptr, 10));
        if (epoch == 0) {
            send_400(request, Error::IncorrectValue, PARAM_EPOCH);
            return;
        }
        if (action == "time") {
            ntp_.set_time(epoch);
        } else if (!ntp_.begin(epoch)) {
            http::reply(request, 500, "text/plain", "unable to listen on udp 123");
            return;
        }
    } else if (action == "stop") {
        ntp_.stop();
    } else if (action == "drop") {
        if (!request->hasParam(PARAM_VALUE, true)) {
            send_400(request, Error::NoFormParam, PARAM_VALUE);
            return;
        }
        ntp_.set_drop(form(request, PARAM_VALUE).toInt() != 0);
    } else {
        send_400(request, Error::IncorrectValue, PARAM_ACTION);
        return;
    }

    http::reply(request, 200, "text/plain", "ok");
}
#endif // ESP32
