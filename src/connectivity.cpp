#include "connectivity.h"

#include "http_util.h"
#include "logging.h"
#include "utils.h"

#if defined(ESP32) && defined(RGB_DEFAULT_PIN)
#include "rgb_led_driver.h"
#elif defined(STATUS_LED_PIN)
#include "gpio_led_driver.h"
#endif

using Pattern = Blinker::Pattern;

// Шаг сетевой части
constexpr uint32_t kTickMs = 10;
using State = WifiPolicy::State;

namespace {

// Платы без светодиода: ритмы отбиваются в пустоту
class NoLed : public LedDriver {
public:
    void show(Rgb /*color*/) override {}
};

std::unique_ptr<LedDriver> make_led(uint8_t brightness) {
#if defined(ESP32) && defined(RGB_DEFAULT_PIN)
    return std::unique_ptr<LedDriver>(new RgbLedDriver<RGB_DEFAULT_PIN>(brightness));
#elif defined(STATUS_LED_PIN)
    (void)brightness;
    // NodeMCU: светодиод на GPIO 2 горит от нуля
    return std::unique_ptr<LedDriver>(new GpioLedDriver(STATUS_LED_PIN, true));
#else
    (void)brightness;
    return std::unique_ptr<LedDriver>(new NoLed());
#endif
}

// "RRGGBB" -> цвет; false - не шесть шестнадцатеричных цифр
bool parse_hex_color(const String &hex, Rgb &out) {
    uint8_t v[3] = {};
    if (hex.length() != 6 || hexText2AsciiArray(hex, v, sizeof(v)) != sizeof(v)) return false;
    out = Rgb{v[0], v[1], v[2]};
    return true;
}

} // namespace

Connectivity::Connectivity(const Config &cfg)
    : cfg_(cfg), link_(cfg.wifi), portal_(link_), led_(make_led(cfg.status_brightness)),
      blinker_(*led_) {}

void Connectivity::begin(AsyncWebServer &server) {
    led_->begin();
    show_status();
    blinker_.loop(millis()); // синий - сразу, а не на первом шаге loop() // синий - сразу, а не на первом loop()

    link_.begin();
    portal_.attach(server); // вместе с onNotFound: редирект на страницу настройки
    attach_rgb(server);
}

void Connectivity::loop() {
    // Сеть и светодиод живут в масштабе сотен миллисекунд: политика считает
    // секунды, самый частый ритм - 250 мс, кнопку держат три секунды. Опрашивать
    // радио на каждом проходе loop() - это десятки тысяч вопросов SDK в секунду
    // впустую; шаг в 10 мс ничего не меняет в поведении. Чтение UART испытуемого
    // остаётся на полной скорости - оно в BenchRoutes.
    const uint32_t now = millis();
    if (!elapsed(now, tick_at_, kTickMs)) return;
    tick_at_ = now;

    link_.loop();
    portal_.loop();
    show_status();
    blinker_.loop(now);
}

void Connectivity::show_status() {
    if (link_.hw_error()) {
        blinker_.set(Rgb::red(), Pattern::Slow);
        return;
    }
    switch (link_.state()) {
    case State::Starting:
    case State::Connecting:
        blinker_.set(Rgb::blue(), Pattern::Slow);
        break;
    case State::Ap:
        blinker_.set(Rgb::blue(), Pattern::Solid);
        break;
    case State::Online:
        blinker_.set(Rgb::green(), Pattern::Heartbeat);
        break;
    case State::Lost:
        blinker_.set(Rgb::red(), Pattern::Fast);
        break;
    }
}

/*
POST /rgb - стенд забирает светодиод себе.

action=begin                  ручной режим: погасить, статус не рисуется
action=brightness&value=0-255 яркость ручного режима
action=color&value=RRGGBB     цвет (после begin)
action=status                 вернуть светодиод индикации режима

Обработчик только ставит флаги: в светодиод цвет уходит из loop().
*/
void Connectivity::attach_rgb(AsyncWebServer &server) {
    server.on("/rgb", HTTP_POST, [this](AsyncWebServerRequest *request) {
        const AsyncWebParameter *action = http::param_any(request, "action");
        if (!action) {
            http::send_400(request, http::Error::NoFormParam, "action");
            return;
        }
        const String a = action->value();
        LOG_INFO("POST /rgb action=" << a);

        if (a == "begin") {
            led_->set_brightness(manual_brightness_.load());
            blinker_.hold(Rgb::off());
            request->send(200, "text/plain", "OK");
            return;
        }
        if (a == "status") {
            led_->set_brightness(cfg_.status_brightness);
            blinker_.release();
            blinker_.refresh();
            request->send(200, "text/plain", "OK");
            return;
        }
        if (a != "brightness" && a != "color") {
            http::send_400(request, http::Error::IncorrectValue, "action");
            return;
        }
        if (!blinker_.held()) {
            http::send_500(request, "RGB not initialized. Call action=begin first");
            return;
        }
        const AsyncWebParameter *value = http::param_any(request, "value");
        if (!value) {
            http::send_400(request, http::Error::NoFormParam, "value");
            return;
        }

        if (a == "brightness") {
            const long v = value->value().toInt();
            if (v < 0 || v > 255) {
                http::send_400(request, http::Error::IncorrectValue, "value");
                return;
            }
            manual_brightness_.store(static_cast<uint8_t>(v));
            led_->set_brightness(static_cast<uint8_t>(v));
            blinker_.refresh();
        } else {
            Rgb c;
            if (!parse_hex_color(value->value(), c)) {
                http::send_400(request, http::Error::IncorrectValue, "value");
                return;
            }
            blinker_.hold(c);
        }
        request->send(200, "text/plain", "OK");
    });
}
