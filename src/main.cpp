#include <Arduino.h>
#include <ESPAsyncWebServer.h>

#include "bench_routes.h"
#include "logging.h"
#include "connectivity.h"

#define VALUE_TO_STRING(x) #x
#define VALUE(x) VALUE_TO_STRING(x)

// UART испытуемого. С ARDUINO_USB_CDC_ON_BOOT=1 (ESP32-C6, нативный USB)
// `Serial` - это USB CDC, консоль METF, а испытуемый висит на Serial0 (UART0).
// На ESP8266 и ESP32 без CDC это один и тот же UART0.
#if defined(ARDUINO_USB_CDC_ON_BOOT) && (ARDUINO_USB_CDC_ON_BOOT)
#define METF_SERIAL Serial0
#else
#define METF_SERIAL Serial
#endif

#ifndef BUTTON_PIN
#define BUTTON_PIN -1
#endif

// Сеть из secrets.ini - по умолчанию; сохранённая через портал её перекрывает
static Connectivity::Config connectivity_config() {
    Connectivity::Config cfg;
    cfg.wifi.ap_prefix = "METF";
    cfg.wifi.default_ssid = VALUE(SSID_NAME);
    cfg.wifi.default_pass = VALUE(SSID_PASS);
    cfg.wifi.button_pin = BUTTON_PIN;
    return cfg;
}

static AsyncWebServer server(80);
static Connectivity net(connectivity_config());
static BenchRoutes bench(METF_SERIAL);

void setup() {
    LOG_BEGIN(115200);
    bench.begin();

#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
    // Нативный USB CDC поднимается уже после старта прошивки, и всё, что
    // напечатано раньше, уходит в никуда. На обычном UART порт готов сразу.
    delay(2000);
#endif
    LOG_INFO("METF version: " << METF_VERSION);

    // Сервер поднимается всегда, есть сеть или нет (docs/wifi.md, P2):
    // подключение идёт дальше в loop()
    net.begin(server);
    bench.attach(server);
    server.begin();
}

void loop() {
    net.loop();
    bench.loop();
}
