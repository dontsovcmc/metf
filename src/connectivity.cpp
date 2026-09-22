#include "connectivity.h"

#include "http_util.h"
#include "logging.h"

#if defined(ESP32) && defined(RGB_DEFAULT_PIN)
#include "rgb_led_driver.h"
#elif defined(STATUS_LED_PIN)
#include "gpio_led_driver.h"
#endif

using Pattern = Blinker::Pattern;
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
#ifndef STATUS_LED_ACTIVE_LOW
#define STATUS_LED_ACTIVE_LOW 1 // NodeMCU: светодиод на GPIO 2 горит от нуля
#endif
    return std::unique_ptr<LedDriver>(new GpioLedDriver(STATUS_LED_PIN, STATUS_LED_ACTIVE_LOW));
#else
    (void)brightness;
    return std::unique_ptr<LedDriver>(new NoLed());
#endif
}

// "RRGGBB" -> цвет; false - не шесть шестнадцатеричных цифр
bool parse_hex_color(const String &hex, Rgb &out) {
    if (hex.length() != 6) return false;
    for (const char c : hex)
        if (!isxdigit(static_cast<unsigned char>(c))) return false;
    const uint32_t v = strtoul(hex.c_str(), nullptr, 16);
    out = Rgb{static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8),
              static_cast<uint8_t>(v)};
    return true;
}

} // namespace

Connectivity::Connectivity(const Config &cfg)
    : cfg_(cfg), link_(cfg.wifi), portal_(link_), led_(make_led(cfg.status_brightness)),
      blinker_(new Blinker(*led_)) {}

void Connectivity::begin(AsyncWebServer &server) {
    led_->begin();
    show_status();
    blinker_->loop(millis()); // синий - сразу, а не на первом loop()

    link_.begin();
    portal_.attach(server);
    attach_rgb(server);
    server.onNotFound([this](AsyncWebServerRequest *request) {
        if (!portal_.handle_not_found(request)) request->send(404, "text/plain", "Not found");
    });
}

void Connectivity::loop() {
    link_.loop();
    portal_.loop();
    show_status();
    blinker_->loop(millis());
}

void Connectivity::show_status() {
    if (link_.hw_error()) {
        blinker_->set(Rgb::red(), Pattern::Slow);
        return;
    }
    switch (link_.state()) {
    case State::Starting:
    case State::Connecting:
        blinker_->set(Rgb::blue(), Pattern::Slow);
        break;
    case State::Ap:
        blinker_->set(Rgb::blue(), Pattern::Solid);
        break;
    case State::Online:
        blinker_->set(Rgb::green(), Pattern::Heartbeat);
        break;
    case State::Lost:
        blinker_->set(Rgb::red(), Pattern::Fast);
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
            rgb_manual_.store(true);
            led_->set_brightness(manual_brightness_.load());
            blinker_->hold(Rgb::off());
            request->send(200, "text/plain", "OK");
            return;
        }
        if (a == "status") {
            rgb_manual_.store(false);
            led_->set_brightness(cfg_.status_brightness);
            blinker_->release();
            blinker_->refresh();
            request->send(200, "text/plain", "OK");
            return;
        }
        if (a != "brightness" && a != "color") {
            http::send_400(request, http::Error::IncorrectValue, "action");
            return;
        }
        if (!rgb_manual_.load()) {
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
            blinker_->refresh();
        } else {
            Rgb c;
            if (!parse_hex_color(value->value(), c)) {
                http::send_400(request, http::Error::IncorrectValue, "value");
                return;
            }
            blinker_->hold(c);
        }
        request->send(200, "text/plain", "OK");
    });
}
