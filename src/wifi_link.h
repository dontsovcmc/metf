#pragma once

/*
Вся работа с радио: подключение к роутеру, своя точка доступа, кнопка.

Решает, что делать, WifiPolicy; WifiLink сообщает ей факты из железа и
исполняет решения. О HTTP и светодиоде не знает: портал и индикация
читают состояние и отдают команды через этот интерфейс.

Потоки. loop() и всё, что трогает радио и флеш, - из loop(). Команды
(request_*) и чтение состояния можно звать из обработчика HTTP: команды
только ставят атомарный флаг, состояние собирается из атомарных полей.

Почему переподключением владеет прошивка, а не ядро, какие особенности
Espressif здесь учтены - docs/wifi.md.
*/

#include <Arduino.h>
#include <IPAddress.h>
#ifdef ESP8266
#include <ESP8266WiFi.h>
#endif

#include <atomic>

#ifdef ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

#include "wifi_policy.h"
#include "wifi_store.h"

class WifiLink {
public:
    struct Config {
        const char *ap_prefix = "ESP";    // имя точки: <prefix>-XXXX, XXXX из MAC
        const char *default_ssid = "";    // вкомпилированная сеть
        const char *default_pass = "";
        int button_pin = -1;              // -1 - кнопки нет
        bool button_active_low = true;
        uint32_t button_hold_ms = 3000;   // удержание до подъёма точки
        WifiPolicy::Config policy;
    };

    struct Status {
        WifiPolicy::State state = WifiPolicy::State::Starting;
        bool connected = false;
        bool from_nvs = false;
        bool fast = false;              // есть пара канал/BSSID
        bool hw_error = false;
        String ssid;
        String ap_ssid;
        bool ap_up = false;
        uint8_t ap_clients = 0;
        IPAddress ip;
        IPAddress ap_ip;
        int8_t rssi = 0;
        uint8_t channel = 0;
        uint32_t offline_s = 0;
        uint32_t failed_attempts = 0;
        int last_reason = 0;
        bool pending = false;           // команда принята, loop() её ещё не исполнил
    };

    struct ScanEntry {
        char ssid[WifiStore::kSsidMax + 1];
        int8_t rssi;
        uint8_t channel;
        bool open;
    };
    static constexpr int kScanMax = 16;

    explicit WifiLink(const Config &cfg);

    WifiLink(const WifiLink &) = delete;
    WifiLink &operator=(const WifiLink &) = delete;
    WifiLink(WifiLink &&) = delete;
    WifiLink &operator=(WifiLink &&) = delete;
    ~WifiLink() = default;

    void begin();
    void loop();

    WifiPolicy::State state() const { return state_.load(); }
    // Точка не поднялась или флеш не записался
    bool hw_error() const { return ap_error_.load() || store_error_.load(); }
    bool ap_active() const;
    Status status() const;

    // --- команды, можно из обработчика HTTP

    // false - SSID 1..32, пароль пустой или 8..63; или прошлая команда ещё не исполнена
    bool request_set(const char *ssid, const char *pass);
    void request_forget() { pending_forget_.store(true); }
    void request_ap() { pending_ap_.store(true); }
    void request_scan() { pending_scan_.store(true); }
    void portal_activity() { activity_.store(true); }

    static bool valid_ssid(const char *ssid);
    static bool valid_pass(const char *pass);

    // Результат последнего скана: копия, до kScanMax сетей, сильные первыми
    int scan_results(ScanEntry *out, int max) const;
    bool scan_running() const { return scan_running_.load(); }

private:
    /*
    Мьютекс для данных, которые loop() пишет, а обработчик HTTP читает
    (скан, креды). На ESP32 - мьютекс FreeRTOS, с наследованием приоритета:
    спинлок на одноядерном C6 мог бы вечно крутиться в задаче async_tcp,
    вытеснившей держателя. На ESP8266 задач нет - и замок не нужен.
    */
    class Mutex {
    public:
#ifdef ESP32
        Mutex() : h_(xSemaphoreCreateMutex()) {}
        void lock() const { xSemaphoreTake(h_, portMAX_DELAY); }
        void unlock() const { xSemaphoreGive(h_); }

    private:
        SemaphoreHandle_t h_;
#else
        void lock() const {}
        void unlock() const {}
#endif
    };

    class Guard {
    public:
        explicit Guard(const Mutex &m) : m_(m) { m_.lock(); }
        ~Guard() { m_.unlock(); }
        Guard(const Guard &) = delete;
        Guard &operator=(const Guard &) = delete;
        Guard(Guard &&) = delete;
        Guard &operator=(Guard &&) = delete;

    private:
        const Mutex &m_;
    };

    void subscribe_events();
    void apply_commands(uint32_t now);
    void poll_button(uint32_t now);
    void poll_scan();
    void execute(WifiPolicy::Action action, uint32_t now);
    void attempt(bool fast);
    void start_ap();
    void stop_ap();
    void follow_channel(uint32_t now);
    void on_connected();
    void note_down(int reason, const char *name);
    void note_up();

    Config cfg_;
    WifiPolicy policy_;
    WifiStore store_;
    char ap_ssid_[24] = {};

    // --- события ядра (задача событий на ESP32)
#ifdef ESP8266
    // Подписка ESP8266 жива, пока жив возвращённый объект
    WiFiEventHandler on_disconnected_;
    WiFiEventHandler on_got_ip_;
#endif
    std::atomic<bool> got_ip_{false};
    std::atomic<int> last_reason_{0};
    std::atomic<uint32_t> down_events_{0};
    uint32_t seen_down_events_ = 0;

    // --- команды из HTTP
    std::atomic<bool> pending_set_{false};
    std::atomic<bool> pending_forget_{false};
    std::atomic<bool> pending_ap_{false};
    std::atomic<bool> pending_scan_{false};
    std::atomic<bool> activity_{false};
    char set_ssid_[WifiStore::kSsidMax + 1] = {};
    char set_pass_[WifiStore::kPassMax + 1] = {};

    // --- состояние для status(), пишется из loop()
    std::atomic<WifiPolicy::State> state_{WifiPolicy::State::Starting};
    std::atomic<bool> ap_error_{false};    // до следующего удачного softAP()
    std::atomic<bool> store_error_{false}; // до перезагрузки
    std::atomic<bool> connected_{false};
    std::atomic<uint32_t> failed_attempts_{0};

    // --- loop()
    bool was_connected_ = false;
    bool attempt_fast_ = false;
    bool radio_off_ = false;         // идёт перезапуск радио
    uint32_t radio_off_at_ = 0;
    uint32_t channel_mismatch_since_ = 0;
    uint8_t channel_mismatch_at_ = 0;
    bool channel_mismatch_ = false;
    uint32_t channel_moved_at_ = 0;

    bool button_down_ = false;
    bool button_fired_ = false;
    uint32_t button_since_ = 0;

    // --- журнал: разрыв сразу, повторы раз в минуту (docs/wifi.md, P1)
    bool down_ = false;
    std::atomic<uint32_t> down_since_{0};
    uint32_t last_report_ = 0;

    // --- скан для портала
    std::atomic<bool> scan_running_{false};
    ScanEntry scan_[kScanMax] = {};
    int scan_count_ = 0;

    Mutex mtx_; // scan_, scan_count_, store_.creds()
};
