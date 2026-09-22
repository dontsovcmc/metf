#include <Arduino.h>
#ifdef ESP32
#include <WiFi.h>
#include <AsyncTCP.h>
#elif defined(ESP8266)
#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#endif
#include <ESPAsyncWebServer.h>


#include "logging.h"
#include "utils.h"

#define VALUE_TO_STRING(x) #x
#define VALUE(x) VALUE_TO_STRING(x)
#define VAR_NAME_VALUE(var) #var "=" VALUE(var)

// Serial port used to talk to the device-under-test (DUT).
// With ARDUINO_USB_CDC_ON_BOOT=1 (ESP32-C6 native USB), `Serial` is the USB CDC,
// so the DUT must be read from Serial0 (UART0 pins). METF's own logs keep using
// `Serial` (USB CDC). On ESP8266 / non-CDC ESP32, Serial0 == Serial == UART0,
// and Serial0 is never referenced on ESP8266 (macro resolves to Serial there).
#if defined(ARDUINO_USB_CDC_ON_BOOT) && (ARDUINO_USB_CDC_ON_BOOT)
  #define METF_SERIAL Serial0
#else
  #define METF_SERIAL Serial
#endif

#include "bench_routes.h"
#include "http_util.h"

static AsyncWebServer server(80);
static BenchRoutes bench(METF_SERIAL);

// Плата, потерявшая сеть, снаружи неотличима от зависшей: она молчит. Разрыв
// печатаем сразу, а повторы - раз в минуту: ядро пробует снова каждые 2-3
// секунды и не прекращает никогда, и без прореживания консоль забьётся.
#define WIFI_REPORT_PERIOD_MS 60000

// Ждём подключения на старте только ради строки с адресом в консоли: сервер
// поднимается в любом случае, а дальше подключается ядро. Дефолтные 60 с - это
// минута без HTTP на плате с неверным паролем (ядро столько молчит о неудаче).
#define WIFI_CONNECT_WAIT_MS 15000

static bool wifi_down = false;
static uint32_t wifi_down_since = 0;
static uint32_t wifi_last_report = 0;
static uint32_t wifi_attempts = 0;

static void wifi_note_down(int reason, const char *name = nullptr) {
    uint32_t now = millis();
    wifi_attempts++;
    if (!wifi_down) {
        wifi_down = true;
        wifi_down_since = now;
        wifi_last_report = now;
        LOG_ERROR("wifi: disconnected, reason " << reason << " " << (name ? name : ""));
    } else if (now - wifi_last_report >= WIFI_REPORT_PERIOD_MS) {
        wifi_last_report = now;
        LOG_ERROR("wifi: offline " << (now - wifi_down_since) / 1000 << " s, "
                  << wifi_attempts << " attempts, last reason " << reason);
    }
}

static void wifi_note_up() {
    if (wifi_down) {
        LOG_INFO("wifi: back after " << (millis() - wifi_down_since) / 1000
                 << " s and " << wifi_attempts << " attempts, ip " << WiFi.localIP());
    }
    wifi_down = false;
    wifi_attempts = 0;
}

#ifdef ESP8266
// Подписка жива, пока жив возвращённый объект
static WiFiEventHandler wifi_on_lost;
static WiFiEventHandler wifi_on_got_ip;
#endif

static void wifi_watch() {
#ifdef ESP32
    WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t info) {
        if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
            wifi_err_reason_t reason = (wifi_err_reason_t)info.wifi_sta_disconnected.reason;
            wifi_note_down(reason, WiFi.disconnectReasonName(reason));
        } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
            wifi_note_up();
        }
    });
#elif defined(ESP8266)
    wifi_on_lost = WiFi.onStationModeDisconnected(
        [](const WiFiEventStationModeDisconnected &event) { wifi_note_down(event.reason); });
    wifi_on_got_ip = WiFi.onStationModeGotIP(
        [](const WiFiEventStationModeGotIP &) { wifi_note_up(); });
#endif
}

// RGB LED Support
#ifdef ESP32
#ifdef RGB_DEFAULT_PIN
#include <FastLED.h>

#ifndef RGB_NUMBER
#define RGB_NUMBER 1 // Maximum supported LEDs (configurable)
#endif

static CRGB rgb_leds[RGB_NUMBER];  // Pre-allocated LED array
static bool rgb_initialized = false; // Initialization state flag
static uint8_t rgb_brightness = 255; // Current brightness (0-255)
#endif // RGB_DEFAULT_PIN
#endif // ESP32 

#ifdef ESP32
#ifdef RGB_DEFAULT_PIN
// Helper: Parse 6-character hex color string to RGB components
// Input: "FF0000" or "ff0000" (red)
// Returns: true if valid, sets r, g, b parameters
bool parseHexColor(const String& hex, uint8_t& r, uint8_t& g, uint8_t& b) {
    if (hex.length() != 6) {
        return false;
    }

    // Validate all characters are hex
    for (char c : hex) {
        if (!isxdigit(c)) {
            return false;
        }
    }

    // Parse RGB components using existing utility functions
    r = (hexCharToInt(hex[0]) << 4) | hexCharToInt(hex[1]);
    g = (hexCharToInt(hex[2]) << 4) | hexCharToInt(hex[3]);
    b = (hexCharToInt(hex[4]) << 4) | hexCharToInt(hex[5]);

    return true;
}

// Helper: Initialize or reinitialize RGB LED strip
// Returns: true if successful, false if parameters invalid
bool rgbBegin(String& error_msg) {
    FastLED.clear();
    // Clear LED array
    memset(rgb_leds, 0, sizeof(rgb_leds));

    // Initialize FastLED with default pin
    // Note: Pin is ignored from parameter, uses compile-time RGB_DEFAULT_PIN instead
    FastLED.addLeds<WS2812B, RGB_DEFAULT_PIN, GRB>(rgb_leds, RGB_NUMBER);

    FastLED.setBrightness(rgb_brightness);

    // Initialize all LEDs to off
    LOCK();
    FastLED.show();
    UNLOCK();

    rgb_initialized = true;
    LOG_INFO("RGB initialized: pin=" << RGB_DEFAULT_PIN << " leds=" << RGB_NUMBER);

    return true;
}
#endif // RGB_DEFAULT_PIN
#endif // ESP32

void setup() {
    LOG_BEGIN(115200);
    bench.begin();

#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
    // Нативный USB CDC поднимается уже после старта прошивки, и всё, что
    // напечатано раньше, уходит в никуда - вместе с адресом платы. На обычном
    // UART порт готов сразу, и ждать незачем.
    delay(2000);
#endif

    LOG_INFO("METF version: " << METF_VERSION);
    LOG_INFO("Welcome to ESP Test Framework. Have a nice tests!");
    // Сеть вкомпилирована в прошивку: увидеть, к какой именно плата идёт,
    // больше негде, а заливка с другими кредами уводит её со стенда молча
    LOG_INFO("Connect to wi-fi ssid: " << VALUE(SSID_NAME));

    wifi_watch();
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();          // начинаем с известного состояния, а не с того,
    delay(300);                 // что осталось от прошлой прошивки
    WiFi.begin(VALUE(SSID_NAME), VALUE(SSID_PASS));
    if (WiFi.waitForConnectResult(WIFI_CONNECT_WAIT_MS) != WL_CONNECTED) {
        // Дальше setup() идёт до конца: переподключение - дело ядра
        // (WiFiSTA::_autoReconnect), а сервер, не поднятый на старте, не
        // поднимется уже никогда, и плата останется доступной только ресетом
        LOG_ERROR("WiFi not connected: starting the server, the core keeps trying");
    }

    // Модем-сон выключен намеренно. По умолчанию станция дремлет между маяками
    // точки, и ответ платы ждёт очередного DTIM: пинг скачет с 6 до 260 мс, а
    // под нагрузкой стенда - когда тесты гасят точку доступа, меняют ей канал и
    // поднимают точку испытуемого - плата теряется совсем. Стенду нужна
    // отзывчивость, а не экономия: питание у METF внешнее, не батарея.
    WiFi.setSleep(false);   // на ESP8266 то же имя есть «для совместимости с ESP32»

    // MAC нужен, чтобы закрепить за платой адрес на роутере: без этого она
    // после каждой перезагрузки берёт что дали, и стенд теряет её молча
    LOG_INFO("IP Address: " << WiFi.localIP() << " MAC: " << WiFi.macAddress()
             << " gateway: " << WiFi.gatewayIP() << " mask: " << WiFi.subnetMask());

    bench.attach(server);

#ifdef ESP32
#ifdef RGB_DEFAULT_PIN
    // POST request to <IP>/rgb
    // action=begin&pin=<gpio>&number=<count>
    // action=brightness&value=<0-255>
    // action=color&value=<RRGGBB>
    server.on("/rgb", HTTP_POST, [](AsyncWebServerRequest *request){
        LOG_INFO("POST /rgb");

        // Log all parameters for debugging
        for (size_t i = 0; i < request->params(); i++) {
            const AsyncWebParameter *param = request->getParam(i);
            if (param) {
                LOG_INFO("  " << param->name() << "=" << param->value());
            }
        }

        // Action parameter is required
        if (!request->hasParam("action", true)) {
            http::send_400(request, http::Error::NoFormParam, "action");
            return;
        }

        String action = request->getParam("action", true)->value();

        // Handle 'begin' action
        if (action == "begin") {
            String error_msg;
            if (!rgbBegin(error_msg)) {
                http::send_500(request, error_msg);
                return;
            }

            request->send(200, "text/plain", "OK");
            return;
        }

        // For brightness and color actions, RGB must be initialized first
        if (!rgb_initialized) {
            http::send_500(request, "RGB not initialized. Call action=begin first");
            return;
        }

        // Handle 'brightness' action
        if (action == "brightness") {
            if (!request->hasParam("value", true)) {
                http::send_400(request, http::Error::NoFormParam, "value");
                return;
            }

            int brightness = request->getParam("value", true)->value().toInt();

            // Validate range
            if (brightness < 0 || brightness > 255) {
                http::send_400(request, http::Error::IncorrectValue, "value");
                return;
            }

            rgb_brightness = (uint8_t)brightness;
            FastLED.setBrightness(rgb_brightness);

            // Update LEDs with thread safety
            LOCK();
            FastLED.show();
            UNLOCK();

            LOG_INFO("RGB brightness set to " << brightness);
            request->send(200, "text/plain", "OK");
            return;
        }

        // Handle 'color' action
        if (action == "color") {
            if (!request->hasParam("value", true)) {
                http::send_400(request, http::Error::NoFormParam, "value");
                return;
            }

            String hex_color = request->getParam("value", true)->value();
            uint8_t r, g, b;

            if (!parseHexColor(hex_color, r, g, b)) {
                http::send_400(request, http::Error::IncorrectValue, "value");
                return;
            }

            // Set all LEDs to the same color
            for (uint8_t i = 0; i < RGB_NUMBER; i++) {
                rgb_leds[i] = CRGB(r, g, b);
            }

            // Update LEDs with thread safety
            LOCK();
            FastLED.show();
            UNLOCK();

            LOG_INFO("RGB color set to #" << hex_color);
            request->send(200, "text/plain", "OK");
            return;
        }

        // Unknown action
        http::send_400(request, http::Error::IncorrectValue, "action");
    });
#endif // RGB_DEFAULT_PIN
#endif // ESP32

    server.onNotFound([](AsyncWebServerRequest *request) {
        request->send(404, "text/plain", "Not found");
    });

    server.begin();
}

void loop() {
    bench.loop();
}
