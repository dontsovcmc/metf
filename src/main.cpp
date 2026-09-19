#include <Arduino.h>
#ifdef ESP32
#include <WiFi.h>
#include <AsyncTCP.h>
#elif defined(ESP8266)
#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#endif
#include <ESPAsyncWebServer.h>
#include <Ticker.h>

#include "Wire.h"

#include "logging.h"
#include "utils.h"
#include "AsyncSerialBuffer.h"
#include "NtpServer.h"

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

AsyncWebServer server(80);
AsyncSerialBuffer asb;

// Импульс отмеряет таймер ядра, а не обработчик запроса: callback сервера
// работает в задаче async_tcp, которая обслуживает все соединения платы, и
// delay() в ней останавливает их все разом (README библиотеки: «You can not
// use yield or delay or any function that uses them inside the callbacks»).
// Импульсы идут по нескольким выводам разом: стенд жмёт кнопку, пока по входу
// счётчика идёт серия. Занятым бывает вывод, а не плата, поэтому слотов
// несколько - по одному таймеру на каждый. Восемь с запасом: стенду хватает
// четырёх (кнопка, сброс, два входа).
#define PULSE_SLOTS 8

static Ticker pulse_timer[PULSE_SLOTS];
static volatile bool pulse_busy[PULSE_SLOTS] = { false };
static uint8_t pulse_pin[PULSE_SLOTS] = { 0 };

static void pulse_end(uint32_t slot) {
    pinMode(pulse_pin[slot], INPUT);   // отпускаем линию в high-Z
    pulse_busy[slot] = false;
}

// Слот, которым сейчас занят этот вывод, или -1
static int pulse_slot_of(uint8_t pin) {
    for (int i = 0; i < PULSE_SLOTS; i++)
        if (pulse_busy[i] && pulse_pin[i] == pin)
            return i;
    return -1;
}

static int pulse_slot_free() {
    for (int i = 0; i < PULSE_SLOTS; i++)
        if (!pulse_busy[i])
            return i;
    return -1;
}

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

#ifdef ESP32
NtpServer ntp;
#endif

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

const char* PARAM_PIN = "pin";
const char* PARAM_VALUE = "value";
const char* PARAM_DURATION_MS = "duration_ms";
const char* PARAM_MSEC = "msec";
const char* PARAM_MODE = "mode";
const char* PARAM_INVERT  = "invert";
const char* PARAM_ACTION = "action";
const char* PARAM_EPOCH = "epoch";
const char* PARAM_SDA_PIN = "sda_pin";
const char* PARAM_SCL_PIN = "scl_pin";
const char* PARAM_ADDRESS = "address";
const char* PARAM_HEXSTRING = "hexstring";
const char* PARAM_LEN = "len";
const char* PARAM_RESPONSE = "response";
const char* PARAM_BAUDRATE = "baudrate";
const char* PARAM_NUMBER = "number";    // For RGB LED count


#define DEFAULT_BAUDRATE 115200
static unsigned long current_baud = DEFAULT_BAUDRATE;

static const uint32_t kAllowedBauds[] = {
  300, 1200, 2400, 4800, 9600, 19200, 38400,
  57600, 74880, 115200, 230400, 250000, 460800, 921600
};

// Параметр из тела POST, а если там нет - из строки запроса. `hasParam(name)`
// без второго аргумента смотрит только строку запроса, поэтому у /serial и
// baudrate, и flush молча не применялись: клиент шлёт их формой в теле.
static const AsyncWebParameter* param_any(AsyncWebServerRequest *request, const char *name) {
    if (request->hasParam(name, true)) return request->getParam(name, true);
    if (request->hasParam(name))       return request->getParam(name);
    return nullptr;
}

bool isAllowedBaud(uint32_t b) {
  for (auto v : kAllowedBauds) if (v == b) return true;
  return false;
}

void notFound (AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "Not found");
}

enum api_error_t {
    NO_GET_PARAM, 
    NO_FORM_PARAM,
    INCORRECT_VALUE
};

void response_400(AsyncWebServerRequest *request, api_error_t err, const String &name)
{   
    switch(err) {
        case NO_GET_PARAM:
            request->send(400, "text/plain", "parameter \'" + name + "\' not found");
            break;
        case NO_FORM_PARAM:
            request->send(400, "text/plain", "post form parameter \'" + name + "\' not found");
            break;
        case INCORRECT_VALUE:
            request->send(400, "text/plain", "parameter \'" + name + "\' is incorrect");
            break;
    }
}

void response_500(AsyncWebServerRequest *request, const String &what)
{
    request->send(500, "text/plain", what);
}

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
    METF_SERIAL.begin(DEFAULT_BAUDRATE);

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

    // GET request to <IP>/ping
    server.on("/ping", HTTP_GET, [](AsyncWebServerRequest *request){
        LOG_INFO("GET /ping");
        request->send(200, "text/plain", "pong");
    });

    // POST request to <IP>/pinMode
    // pin=<number>
    // mode=<INPUT,OUTPUT,INPUT_PULLUP> integer constants
    server.on("/pinMode", HTTP_POST, [](AsyncWebServerRequest *request) {
        /*int headers = request->headers();
        int i;
        for(i=0;i<headers;i++){
            AsyncWebHeader* h = request->getHeader(i);
            Serial.printf("HEADER[%s]: %s\n", h->name().c_str(), h->value().c_str());
        } */
        if (!request->hasParam(PARAM_PIN, true)) {
            response_400(request, NO_FORM_PARAM, PARAM_PIN);
            return;
        }
        if (!request->hasParam(PARAM_MODE, true)) {
            response_400(request, NO_FORM_PARAM, PARAM_MODE);
            return;
        }

        uint8_t pin = request->getParam(PARAM_PIN, true)->value().toInt();
        uint8_t mode = request->getParam(PARAM_MODE, true)->value().toInt();

        pinMode(pin, mode);
        request->send(200, "text/plain", "OK");
    });

    // Send a GET request to <IP>/digitalRead?pin=<number>
    server.on("/digitalRead", HTTP_GET, [] (AsyncWebServerRequest *request) {

        if (!request->hasParam(PARAM_PIN)) {
            response_400(request, NO_GET_PARAM, PARAM_PIN);
            return;
        }

        uint8_t pin = request->getParam(PARAM_PIN)->value().toInt();

        //TODO check pin 0 - x
        if (digitalRead(pin) == HIGH) {
            request->send(200, "text/plain", "1");
        } else {
            request->send(200, "text/plain", "0");
        }
    });

    // POST request to <IP>/digitalWrite 
    // form fields: 
    // pin=<number>
    // value=<HIGH, LOW> constants
    server.on("/digitalWrite", HTTP_POST, [](AsyncWebServerRequest *request){
        
        if (!request->hasParam(PARAM_PIN, true)) {
            response_400(request, NO_FORM_PARAM, PARAM_PIN);
            return;
        }

        if (!request->hasParam(PARAM_VALUE, true)) {
            response_400(request, NO_FORM_PARAM, PARAM_VALUE);
            return;
        }

        uint8_t pin = request->getParam(PARAM_PIN, true)->value().toInt();
        uint8_t value = request->getParam(PARAM_VALUE, true)->value().toInt();

        digitalWrite(pin, value);
        request->send(200, "text/plain", "OK");
    });

    // POST /pulse  form: pin=<n>&duration_ms=<ms>&value=<0|1>
    server.on("/pulse", HTTP_POST, [](AsyncWebServerRequest *request){

        if (!request->hasParam(PARAM_PIN, true)) {
            response_400(request, NO_FORM_PARAM, PARAM_PIN);
            return;
        }

        if (!request->hasParam(PARAM_DURATION_MS, true)) {
            response_400(request, NO_FORM_PARAM, PARAM_DURATION_MS);
            return;
        }

        if (!request->hasParam(PARAM_VALUE, true)) {
            response_400(request, NO_FORM_PARAM, PARAM_VALUE);
            return;
        }

        uint8_t pin   = request->getParam(PARAM_PIN, true)->value().toInt();
        uint8_t value = request->getParam(PARAM_VALUE, true)->value().toInt();
        uint32_t ms   = request->getParam(PARAM_DURATION_MS, true)->value().toInt();

        // Отказ - про вывод, а не про плату: по соседнему выводу импульс идти
        // может и должен
        if (pulse_slot_of(pin) >= 0) {
            request->send(409, "text/plain", "pulse in progress");
            return;
        }

        int slot = pulse_slot_free();
        if (slot < 0) {
            request->send(503, "text/plain", "no free pulse timer");
            return;
        }

        pulse_pin[slot] = pin;
        pulse_busy[slot] = true;
        pinMode(pin, OUTPUT);
        digitalWrite(pin, value);
#ifdef ESP8266
        // не SYS-контекст, а loop(); лямбда со слотом подходит, потому что
        // здешний Ticker берёт std::function
        pulse_timer[slot].once_ms_scheduled(ms, [slot]() { pulse_end((uint32_t)slot); });
#else
        // здешний Ticker берёт только указатель на функцию, зато с аргументом
        pulse_timer[slot].once_ms<uint32_t>(ms, pulse_end, (uint32_t)slot);
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
        request->send(202, "text/plain", String(ms));
    });
  
    server.on("/i2c", HTTP_POST, [](AsyncWebServerRequest *request){
        String action, hexstring;
        uint8_t sda_pin = SDA, scl_pin = SCL, b, address, len;
        int err, i;

        if (!request->hasParam(PARAM_ACTION, true)) {
            response_400(request, NO_FORM_PARAM, PARAM_ACTION);
            return;
        }
        
        LOG_INFO("POST /i2c");
        for (size_t i = 0; i < request->params(); i++) {
            const AsyncWebParameter *param = request->getParam(i);
            if (param) {
                LOG_INFO("  " << param->name() << "=" << param->value());
            }
        }

        action = request->getParam(PARAM_ACTION, true)->value();

        if (action == "begin") {
            if (request->hasParam(PARAM_SDA_PIN, true) 
                && request->hasParam(PARAM_SCL_PIN, true)) {
                sda_pin = request->getParam(PARAM_SDA_PIN, true)->value().toInt();
                scl_pin = request->getParam(PARAM_SCL_PIN, true)->value().toInt();
            }

            LOG_INFO("Wire begin SDA=" << sda_pin << " SCL=" << scl_pin);
            Wire.begin(sda_pin, scl_pin);

        } else if (action == "setClock") {
            if (!request->hasParam(PARAM_VALUE, true)) {
                response_400(request, NO_FORM_PARAM, PARAM_VALUE);
                return;
            }

            uint32_t clock = request->getParam(PARAM_VALUE, true)->value().toInt();
            LOG_INFO("Wire.setClock(" << clock << ")");

            Wire.setClock(clock);

        } else if (action == "setClockStretchLimit") {

            if (!request->hasParam(PARAM_VALUE, true)) {
                response_400(request, NO_FORM_PARAM, PARAM_VALUE);
                return;
            }

            uint32_t stretch = request->getParam(PARAM_VALUE, true)->value().toInt();

            #ifdef ESP8266
            // ESP8266: setClockStretchLimit takes microseconds
            LOG_INFO("Wire.setClockStretchLimit(" << stretch << " us)");
            Wire.setClockStretchLimit(stretch);
            #elif defined(ESP32)
            // ESP32/ESP32-C6: setTimeOut takes milliseconds
            // Convert microseconds to milliseconds, minimum 1000ms
            uint32_t timeout_ms = stretch / 1000;
            if (timeout_ms < 1000) {
                timeout_ms = 1000; // Minimum threshold for ESP32
            }
            LOG_INFO("Wire.setTimeOut(" << timeout_ms << " ms) [converted from " << stretch << " us]");
            Wire.setTimeOut(timeout_ms);
            #endif

        } else if (action == "ask") {
            
            if (!request->hasParam(PARAM_ADDRESS, true)) {
                response_400(request, NO_FORM_PARAM, PARAM_ADDRESS);
                return;
            }
            if (!request->hasParam(PARAM_HEXSTRING, true)) {
                response_400(request, NO_FORM_PARAM, PARAM_HEXSTRING);
                return;
            }
            if (!request->hasParam(PARAM_RESPONSE, true)) {
                response_400(request, NO_FORM_PARAM, PARAM_RESPONSE);
                return;
            }

            uint8_t response_len = request->getParam(PARAM_RESPONSE, true)->value().toInt();
            address = request->getParam(PARAM_ADDRESS, true)->value().toInt();
            hexstring = request->getParam(PARAM_HEXSTRING, true)->value();

            uint8_t arr[hexstring.length()];  //2

            len = hexText2AsciiArray(hexstring, arr, hexstring.length());

            if (len == 0) {
                response_400(request, INCORRECT_VALUE, PARAM_HEXSTRING);
                return;
            }

            Wire.beginTransmission(address);
            for(int i=0; i<len; i++) {
                LOG_DEBUG("i2c > " << String(arr[i], 16));
                if (Wire.write(arr[i]) != 1) {
                    Wire.endTransmission();
                    response_500(request, "i2c write error");
                    return;
                }
            }

            err = Wire.endTransmission();
            if (err != 0) {
                response_500(request, "i2c end transmission error " + String(err));
                /* https://www.arduino.cc/en/Reference/WireEndTransmission
                0:success
                1:data too long to fit in transmit buffer
                2:received NACK on transmit of address
                3:received NACK on transmit of data
                4:other error
                */
                return;
            }  
            
            LOG_INFO("Wire send: " << hexstring << " " << len << " bytes to device " << address);
            delay(1); // Дадим время подумать 

            hexstring.clear();
            hexstring.reserve(response_len * 2 + 1);   //TODO is null terminator needed?

            i = 0;
            while (i < response_len) {
                if (Wire.requestFrom(address, (uint8_t)1) != (uint8_t)1) {
                    response_500(request, "i2c read timeout. Received: " + hexstring);
                    return;
                }
                b = Wire.read();
                LOG_DEBUG("i2c < " << String(b, 16));

                hexstring += intToHexChar(b >> 4);
                hexstring += intToHexChar(b & 0x0F);
                i++;
            }
            
            
            LOG_INFO("Received: " << hexstring);

            request->send(200, "text/plain", hexstring);
            return;

        } else if (action == "flush") {
            Wire.flush();
            request->send(200, "text/plain", "OK");

        } else {
            response_400(request, INCORRECT_VALUE, PARAM_ACTION);
        }
        request->send(200, "text/plain", "OK");
    });

    // POST request to <IP>/serial
    // baudrate=<baudrate>
    server.on("/serial", HTTP_POST, [](AsyncWebServerRequest* request){
        String out;
        uint32_t nb = DEFAULT_BAUDRATE;
        const AsyncWebParameter *baud = param_any(request, PARAM_BAUDRATE);
        if (baud) {
            nb = (uint32_t) baud->value().toInt();
        }

        if (nb == 0 || !isAllowedBaud(nb)) {
            request->send(400, "text/plain; charset=utf-8", "Invalid speed");
            return;
        }

        if (nb != current_baud) {
            // Короткая критическая секция: останавливаем приём и переключаем UART
            LOCK();
            METF_SERIAL.end();
            METF_SERIAL.begin(nb);
            current_baud = nb;
            UNLOCK();

            out = "Set " + String(nb) + " baudrate";
        } else {
            out = "Baudrate is " + String(nb);
        }

        const AsyncWebParameter *flush = param_any(request, "flush");
        if (flush && flush->value() == "1") {
            asb.flush();
            out += ", flush buffer";
        }
        
        request->send(200, "text/plain; charset=utf-8", out);
    });


    // GET request to <IP>/read/stat
    // состояние кольца лога: сколько строк лежит, сколько вытеснено, на какой
    // скорости читаем UART. dropped > 0 - в логе дыра, читателю верить нельзя.
    //
    // Регистрируется ДО /read: обработчик подходит и по префиксу
    // (`url.startsWith(_uri + "/")` в AsyncCallbackWebHandler::canHandle),
    // поэтому /read, объявленный первым, перехватил бы и /read/stat.
    server.on("/read/stat", HTTP_GET, [](AsyncWebServerRequest *request){

        String out = "{\"lines\":" + String((uint32_t)asb.count())
                   + ",\"dropped\":" + String(asb.dropped())
                   + ",\"baud\":" + String(current_baud)
                   + ",\"capacity\":" + String((uint32_t)(ASB_MAX_LINES - 1))
                   + ",\"line_len\":" + String((uint32_t)ASB_MAX_LINE_LEN)
                   + ",\"bytes\":" + String((uint32_t)ASB_MAX_LINES * ASB_MAX_LINE_LEN)
                   + "}";
        request->send(200, "application/json", out);
    });

    server.on("/read", HTTP_GET, [](AsyncWebServerRequest *request){

        AsyncResponseStream* res = request->beginResponseStream("text/plain; charset=utf-8");
        // Слить накопленные строки без добавления разделителей
        asb.drain_to(*res);
        request->send(res);
    });

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
        if (!request->hasParam(PARAM_ACTION, true)) {
            response_400(request, NO_FORM_PARAM, PARAM_ACTION);
            return;
        }

        String action = request->getParam(PARAM_ACTION, true)->value();

        // Handle 'begin' action
        if (action == "begin") {
            String error_msg;
            if (!rgbBegin(error_msg)) {
                response_500(request, error_msg);
                return;
            }

            request->send(200, "text/plain", "OK");
            return;
        }

        // For brightness and color actions, RGB must be initialized first
        if (!rgb_initialized) {
            response_500(request, "RGB not initialized. Call action=begin first");
            return;
        }

        // Handle 'brightness' action
        if (action == "brightness") {
            if (!request->hasParam(PARAM_VALUE, true)) {
                response_400(request, NO_FORM_PARAM, PARAM_VALUE);
                return;
            }

            int brightness = request->getParam(PARAM_VALUE, true)->value().toInt();

            // Validate range
            if (brightness < 0 || brightness > 255) {
                response_400(request, INCORRECT_VALUE, PARAM_VALUE);
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
            if (!request->hasParam(PARAM_VALUE, true)) {
                response_400(request, NO_FORM_PARAM, PARAM_VALUE);
                return;
            }

            String hex_color = request->getParam(PARAM_VALUE, true)->value();
            uint8_t r, g, b;

            if (!parseHexColor(hex_color, r, g, b)) {
                response_400(request, INCORRECT_VALUE, PARAM_VALUE);
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
        response_400(request, INCORRECT_VALUE, PARAM_ACTION);
    });
#endif // RGB_DEFAULT_PIN
#endif // ESP32

#ifdef ESP32
    /*
    GET <IP>/ntp/stat - состояние сервера времени.

    Объявлен раньше /ntp: canHandle у AsyncCallbackWebHandler совпадает и по
    префиксу, и объявленный первым /ntp проглотил бы /ntp/stat.
    */
    server.on("/ntp/stat", HTTP_GET, [](AsyncWebServerRequest *request){

        NtpServer::Stat st = ntp.stat();
        String out = String("{\"running\":") + (ntp.running() ? "true" : "false")
                   + ",\"dropping\":" + (ntp.dropping() ? "true" : "false")
                   + ",\"epoch\":" + String(ntp.now_epoch())
                   + ",\"requests\":" + String(st.requests)
                   + ",\"replies\":" + String(st.replies)
                   + ",\"dropped\":" + String(st.dropped)
                   + ",\"ignored\":" + String(st.ignored)
                   + ",\"last_epoch\":" + String(st.last_epoch)
                   + ",\"last_client\":\"" + st.last_client.toString() + "\""
                   + "}";
        request->send(200, "application/json", out);
    });

    /*
    POST <IP>/ntp - сервер времени стенда.

    action=start&epoch=<unix>  начать отвечать, назначив время
    action=time&epoch=<unix>   переставить часы, не трогая слушателя
    action=stop                перестать слушать: клиенту придёт отказ порта
    action=drop&value=<0|1>    слушать, но молчать: клиент дождётся таймаута
    */
    server.on("/ntp", HTTP_POST, [](AsyncWebServerRequest *request){

        if (!request->hasParam(PARAM_ACTION, true)) {
            response_400(request, NO_FORM_PARAM, PARAM_ACTION);
            return;
        }
        String action = request->getParam(PARAM_ACTION, true)->value();
        LOG_INFO("POST /ntp action=" << action);

        if (action == "start" || action == "time") {
            if (!request->hasParam(PARAM_EPOCH, true)) {
                response_400(request, NO_FORM_PARAM, PARAM_EPOCH);
                return;
            }
            uint32_t epoch = (uint32_t)strtoul(
                request->getParam(PARAM_EPOCH, true)->value().c_str(), NULL, 10);
            if (epoch == 0) {
                response_400(request, INCORRECT_VALUE, PARAM_EPOCH);
                return;
            }
            if (action == "time") {
                ntp.set_time(epoch);
            } else if (!ntp.begin(epoch)) {
                request->send(500, "text/plain", "unable to listen on udp 123");
                return;
            }
        }
        else if (action == "stop") {
            ntp.stop();
        }
        else if (action == "drop") {
            if (!request->hasParam(PARAM_VALUE, true)) {
                response_400(request, NO_FORM_PARAM, PARAM_VALUE);
                return;
            }
            ntp.set_drop(request->getParam(PARAM_VALUE, true)->value().toInt() != 0);
        }
        else {
            response_400(request, INCORRECT_VALUE, PARAM_ACTION);
            return;
        }

        request->send(200, "text/plain", "ok");
    });
#endif // ESP32

    // GET request to <IP>/version
    // read framework version
    server.on("/version", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(200, "text/plain", METF_VERSION);
    });

    server.onNotFound(notFound);

    server.begin();
}

void loop() {
    while (METF_SERIAL.available() > 0) {
        asb.pushChar((char)METF_SERIAL.read());
    }
}