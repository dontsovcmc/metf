#include "wifi_link.h"

#include <algorithm>
#include <cstring>

#ifdef ESP32
#include <WiFi.h>
#include <esp_wifi.h>
#endif

#include "logging.h"
#include "timing.h"
#include "wifi_reason.h"

using Action = WifiPolicy::Action;
using State = WifiPolicy::State;

namespace {

// Консоль при потере сети: первый разрыв сразу, дальше сводка раз в минуту -
// попытки идут каждые несколько секунд, и без прореживания консоль забьётся
constexpr uint32_t kReportPeriodMs = 60000;

// Перезапуск радио: сколько держать выключенным. Ждём не delay(), а в loop():
// за 200 мс UART испытуемого на 115200 приносит больше, чем держит буфер
constexpr uint32_t kRadioOffMs = 200;

// Снимок состояния для HTTP собираем не каждый проход: он стоит нескольких
// вопросов к радио и пары строк, а меняется в нём раз в секунду
constexpr uint32_t kStatusPeriodMs = 500;

// Точка следует за каналом радио: несовпадение должно продержаться, а
// переезды - не чаще. softAP() переинициализирует интерфейс и прерывает маяки
constexpr uint32_t kChannelSettleMs = 3000;
constexpr uint32_t kChannelMoveEveryMs = 10000;

constexpr uint8_t kApMaxClients = 4;

bool valid_channel(int ch) { return ch >= 1 && ch <= 13; }

// Канал, на котором сейчас радио, или 0. WiFi.channel() на обоих ядрах -
// это и есть esp_wifi_get_channel (ESP32) / текущий канал станции (8266)
uint8_t radio_channel() {
    const int ch = WiFi.channel();
    return valid_channel(ch) ? static_cast<uint8_t>(ch) : 0;
}

} // namespace

WifiLink::WifiLink(const Config &cfg)
    : cfg_(cfg), policy_(cfg.policy), store_(cfg.default_ssid, cfg.default_pass) {}

bool WifiLink::valid_ssid(const char *ssid) {
    const size_t n = ssid ? strlen(ssid) : 0;
    return n >= 1 && n <= WifiStore::kSsidMax;
}

bool WifiLink::valid_pass(const char *pass) {
    const size_t n = pass ? strlen(pass) : 0;
    return n == 0 || (n >= 8 && n <= WifiStore::kPassMax);
}

// ---------------------------------------------------------------- старт

void WifiLink::begin() {
    // Креды живут в WifiStore, а не в конфиге SDK: иначе каждый begin()
    // писал бы их во флеш второй раз, и плата после перепрошивки могла бы
    // подключиться к сети, о которой прошивка не знает
    WiFi.persistent(false);
    // Переподключается политика. Автоповтор ядра поверх неё гонялся бы с ней,
    // а на AUTH_FAIL ядро замолкает насовсем (docs/wifi.md, известные дыры)
    WiFi.setAutoReconnect(false);
    subscribe_events();
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false); // docs/wifi.md, P6: отзывчивость важнее экономии

    if (!store_.begin()) store_error_.store(true);

    uint8_t mac[6] = {};
    WiFi.macAddress(mac);
    snprintf(ap_ssid_, sizeof(ap_ssid_), "%s-%02X%02X", cfg_.ap_prefix, mac[4], mac[5]);

    if (cfg_.button_pin >= 0)
        pinMode(cfg_.button_pin, cfg_.button_active_low ? INPUT_PULLUP : INPUT);

    const WifiStore::Creds &c = store_.creds();
    // Сеть и MAC печатаем при старте: увидеть, к какой сети идёт плата, больше
    // негде, а MAC нужен, чтобы закрепить за ней адрес на роутере
    LOG_INFO("wifi: ssid " << (c.empty() ? "<none>" : c.ssid) << " ("
                           << (c.from_nvs ? "saved" : "build") << "), fast "
                           << (store_.fast().valid() ? "ch " + String(store_.fast().channel)
                                                     : String("none"))
                           << ", MAC " << WiFi.macAddress() << ", own AP " << ap_ssid_);
}

void WifiLink::subscribe_events() {
#ifdef ESP32
    WiFi.onEvent([this](arduino_event_id_t event, arduino_event_info_t info) {
        switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            got_ip_.store(true);
            break;
        case ARDUINO_EVENT_WIFI_STA_LOST_IP:
        case ARDUINO_EVENT_WIFI_STA_STOP:
            got_ip_.store(false);
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            got_ip_.store(false);
            if (!take(self_down_)) last_reason_.store(info.wifi_sta_disconnected.reason);
            down_events_.store(down_events_.load() + 1); // пишет только задача событий
            break;
        default:
            break;
        }
    });
#elif defined(ESP8266)
    on_disconnected_ =
        WiFi.onStationModeDisconnected([this](const WiFiEventStationModeDisconnected &e) {
            got_ip_.store(false);
            last_reason_.store(e.reason);
            down_events_.store(down_events_.load() + 1); // пишет только задача событий
        });
    on_got_ip_ = WiFi.onStationModeGotIP(
        [this](const WiFiEventStationModeGotIP &) { got_ip_.store(true); });
#endif
}

// ---------------------------------------------------------------- loop

void WifiLink::loop() {
    const uint32_t now = millis();

    if (radio_off_) {
        if (!elapsed(now, radio_off_at_, kRadioOffMs)) return;
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);
        radio_off_ = false;
        if (radio_off_ap_) start_ap(); // точку гасило выключение радио, а не политика
    }

    apply_commands(now);
    poll_button(now);
    poll_scan();

    const bool ap_up = ap_active();
    ap_up_.store(ap_up);
    WifiPolicy::Facts f;
    f.has_creds = !store_.creds().empty();
    f.has_fast = store_.fast().valid();
    f.connected = got_ip_.load() && WiFi.status() == WL_CONNECTED;
    f.ap_up = ap_up;
    f.ap_clients = ap_up ? WiFi.softAPgetStationNum() : 0;

    const uint32_t down_events = down_events_.load();
    if (down_events != seen_down_events_) {
        seen_down_events_ = down_events;
        note_down(last_reason_.load());
    }

    if (f.connected && !was_connected_) on_connected();
    if (!f.connected && was_connected_ && !down_) note_down(last_reason_.load());
    was_connected_ = f.connected;
    connected_.store(f.connected);

    execute(policy_.tick(now, f), now);
    state_.store(policy_.state());
    failed_attempts_.store(policy_.failed_attempts());

    follow_channel(now);
    publish_status(now, f);
}

void WifiLink::apply_commands(uint32_t now) {
    if (pending_set_.load()) {
        const bool ok = store_.save_creds(set_ssid_, set_pass_);
        if (!ok) {
            store_error_.store(true);
            LOG_ERROR("wifi: new network kept in RAM only, flash write failed");
        }
        LOG_INFO("wifi: new network " << set_ssid_ << ", connecting");
        last_reason_.store(0);   // причина от прошлой сети к новой не относится
        memset(set_pass_, 0, sizeof(set_pass_));
        pending_set_.store(false);
        policy_.request_connect();
    }

    if (take(pending_forget_)) {
        if (!store_.forget()) store_error_.store(true);
        LOG_INFO("wifi: saved network forgotten, back to build network "
                 << (store_.creds().empty() ? "<none>" : store_.creds().ssid));
        policy_.request_connect();
    }

    if (take(pending_ap_)) policy_.request_ap(true);
    if (take(pending_ap_off_)) policy_.request_ap(false);
    if (take(activity_)) policy_.portal_activity(now);
}

void WifiLink::poll_button(uint32_t now) {
    if (cfg_.button_pin < 0) return;
    const bool pressed = (digitalRead(cfg_.button_pin) == LOW) == cfg_.button_active_low;
    if (!pressed) {
        button_down_ = false;
        return;
    }
    if (!button_down_) {
        button_down_ = true;
        button_fired_ = false;
        button_since_ = now;
    } else if (!button_fired_ && elapsed(now, button_since_, cfg_.button_hold_ms)) {
        button_fired_ = true;
        LOG_INFO("wifi: button held, raising own access point");
        policy_.request_ap();
    }
}

void WifiLink::execute(Action action, uint32_t now) {
    switch (action) {
    case Action::None:
        break;
    case Action::AttemptFast:
        attempt(true);
        break;
    case Action::AttemptScan:
        attempt(false);
        break;
    case Action::StartAp:
        start_ap();
        break;
    case Action::StopAp:
        stop_ap();
        break;
    case Action::RestartRadio:
        LOG_INFO("wifi: restarting radio");
        radio_off_ap_ = ap_active(); // погаснет вместе с радио - поднимем заново
        self_down_.store(true);      // разрыв наш: причиной его не считаем
        WiFi.mode(WIFI_OFF);
        radio_off_ = true;
        radio_off_at_ = now;
        break;
    case Action::ForgetFast: {
        if (!store_.clear_fast()) store_error_.store(true);
        LOG_INFO("wifi: saved channel did not work twice, forgotten");
        break;
    }
    }
}

// ---------------------------------------------------------------- действия

void WifiLink::attempt(bool fast) {
    const WifiStore::Creds &c = store_.creds();
    const WifiStore::Fast &f = store_.fast();

    // Прошлая попытка могла ещё идти: обрываем её сами. Arduino disconnect()
    // тут не годится - он молча выходит, если связи ещё нет
    self_down_.store(true);          // разрыв наш: причиной его не считаем
#ifdef ESP32
    esp_wifi_disconnect();
#else
    WiFi.disconnect(false);
#endif
    if ((WiFi.getMode() & WIFI_STA) == 0) WiFi.mode(ap_active() ? WIFI_AP_STA : WIFI_STA);

    attempt_fast_ = fast && f.valid();
    if (attempt_fast_) {
        LOG_INFO("wifi: connecting to " << c.ssid << ", fast ch " << f.channel);
        WiFi.begin(c.ssid, c.pass, f.channel, f.bssid);
    } else {
        LOG_INFO("wifi: connecting to " << c.ssid << ", scan");
        WiFi.begin(c.ssid, c.pass);
    }
}

void WifiLink::start_ap() {
    if (ap_active()) return;

    // Канал - тот, на котором радио уже стоит: одно радио не держит AP и STA
    // на разных каналах, и точка на чужом канале молчит (docs/wifi.md)
    uint8_t ch = radio_channel();
    if (ch == 0 && store_.fast().valid()) ch = store_.fast().channel;
    if (ch == 0) ch = 1;

    WiFi.mode(WIFI_AP_STA);
    if (!WiFi.softAP(ap_ssid_, nullptr, ch, 0, kApMaxClients)) {
        ap_error_.store(true);
        LOG_ERROR("wifi: own access point " << ap_ssid_ << " failed to start");
        return;
    }
    WiFi.setSleep(false);
    ap_error_.store(false);
    ap_channel_ = ch;
    ap_ip_ = WiFi.softAPIP();
    LOG_INFO("wifi: own access point " << ap_ssid_ << " up, ch " << ch << ", open http://"
                                        << WiFi.softAPIP() << "/");
}

void WifiLink::stop_ap() {
    WiFi.softAPdisconnect(true);
    WiFi.setSleep(false);
    LOG_INFO("wifi: own access point stopped");
}

bool WifiLink::ap_active() const { return (WiFi.getMode() & WIFI_AP) != 0; }

void WifiLink::follow_channel(uint32_t now) {
#ifdef ESP32
    // Во время попытки и скана радио прыгает по каналам - это не повод
    // двигать точку
    if (!ap_active() || policy_.attempting() || scan_running_.load()) {
        channel_mismatch_ = false;
        return;
    }
    const uint8_t radio = radio_channel();
    if (radio == 0 || radio == ap_channel_) {
        channel_mismatch_ = false;
        return;
    }
    // Отсчёт - для одного и того же чужого канала: иначе «три секунды
    // несовпадения» набираются из разных каналов, по которым идёт скан
    if (!channel_mismatch_ || radio != channel_mismatch_at_) {
        channel_mismatch_ = true;
        channel_mismatch_at_ = radio;
        channel_mismatch_since_ = now;
        return;
    }
    if (!elapsed(now, channel_mismatch_since_, kChannelSettleMs) ||
        !elapsed(now, channel_moved_at_, kChannelMoveEveryMs))
        return;

    // Радио ушло на канал роутера, а точка осталась на старом: маяки не идут,
    // хотя softAP() вернул true. Переносим точку туда, где радио.
    LOG_INFO("wifi: own access point follows radio, ch " << ap_channel_ << " -> " << radio);
    WiFi.softAP(ap_ssid_, nullptr, radio, 0, kApMaxClients);
    ap_channel_ = radio;
    channel_moved_at_ = now;
    channel_mismatch_ = false;
#else
    (void)now; // ESP8266: SDK сам держит точку на канале станции
#endif
}

void WifiLink::on_connected() {
    note_up();
    last_reason_.store(0);   // прошлая беда кончилась, и в JSON ей не место

    WifiStore::Fast f;
    f.channel = static_cast<uint8_t>(WiFi.channel());
    // BSSID() отдаёт NULL, если станция успела отвалиться между проверкой
    // связи и этим вызовом: esp_wifi_sta_get_ap_info вернёт NOT_CONNECT
    const uint8_t *bssid = WiFi.BSSID();
    if (bssid != nullptr) memcpy(f.bssid, bssid, sizeof(f.bssid));
    if (bssid != nullptr && f.valid() && !(f == store_.fast())) {
        if (!store_.save_fast(f)) store_error_.store(true);
        LOG_INFO("wifi: channel " << f.channel << " saved for fast connect");
    }
}

// ---------------------------------------------------------------- скан для портала

void WifiLink::poll_scan() {
    // Скан, пока станция подключается, ядро отвергает: ждём конца попытки
    if (pending_scan_.load() && !scan_running_.load() && !policy_.attempting() && !radio_off_) {
        pending_scan_.store(false);
        const int16_t r = WiFi.scanNetworks(true);
        scan_running_.store(r == WIFI_SCAN_RUNNING);
        return;
    }
    if (!scan_running_.load()) return;

    const int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;
    scan_running_.store(false);
    if (n < 0) return;

    ScanEntry found[kScanMax] = {};
    int count = 0;
    for (int16_t i = 0; i < n; i++) {
        const String ssid = WiFi.SSID(i);
        if (ssid.isEmpty()) continue; // скрытая сеть
        const auto rssi = static_cast<int8_t>(WiFi.RSSI(i));

        // Одна строка на сеть: у сети с несколькими точками - самая сильная
        int at = -1;
        for (int k = 0; k < count; k++)
            if (ssid == found[k].ssid) at = k;
        if (at >= 0) {
            if (rssi <= found[at].rssi) continue;
        } else if (count < kScanMax) {
            at = count++;
        } else {
            continue;
        }
        strlcpy(found[at].ssid, ssid.c_str(), sizeof(found[at].ssid));
        found[at].rssi = rssi;
        found[at].channel = static_cast<uint8_t>(WiFi.channel(i));
#ifdef ESP32
        found[at].open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
#else
        found[at].open = WiFi.encryptionType(i) == ENC_TYPE_NONE;
#endif
    }
    WiFi.scanDelete();

    // Сильные сверху. Границу пишем явно: из цикла выше компилятор её не
    // выводит и ругается (-Warray-bounds) на развёрнутую сортировку.
    const int total = count < kScanMax ? count : kScanMax;
    std::sort(found, found + total,
              [](const ScanEntry &a, const ScanEntry &b) { return a.rssi > b.rssi; });

    const std::lock_guard<Mutex> g(mtx_);
    memcpy(scan_, found, sizeof(scan_));
    scan_count_ = count;
}

int WifiLink::scan_results(ScanEntry *out, int max) const {
    const std::lock_guard<Mutex> g(mtx_);
    const int n = scan_count_ < max ? scan_count_ : max;
    memcpy(out, scan_, sizeof(ScanEntry) * n);
    return n;
}

// ---------------------------------------------------------------- команды и состояние

bool WifiLink::request_set(const char *ssid, const char *pass) {
    if (!valid_ssid(ssid) || !valid_pass(pass)) return false;
    if (pending_set_.load()) return false;
    strlcpy(set_ssid_, ssid, sizeof(set_ssid_));
    strlcpy(set_pass_, pass ? pass : "", sizeof(set_pass_));
    pending_set_.store(true); // после записи буферов: loop() прочтёт их целыми
    return true;
}

WifiLink::Status WifiLink::status() const {
    const std::lock_guard<Mutex> g(mtx_);
    return status_;
}

/*
Снимок состояния для обработчиков HTTP.

Собирается здесь, в loop(), потому что почти каждое поле - вопрос к радио,
а задача сервера обслуживает все соединения платы: спрашивать радио оттуда
значит занимать её на время ответа SDK.
*/
void WifiLink::publish_status(uint32_t now, const WifiPolicy::Facts &f) {
    const bool changed = f.connected != status_.connected || f.ap_up != status_.ap_up ||
                         f.ap_clients != status_.ap_clients ||
                         policy_.state() != status_.state ||
                         scan_running_.load() != status_.scanning;
    if (!changed && !elapsed(now, status_at_, kStatusPeriodMs)) return;
    status_at_ = now;

    Status s;
    s.state = policy_.state();
    s.connected = f.connected;
    s.sta_up = (WiFi.getMode() & WIFI_STA) != 0;
    s.hw_error = hw_error();
    s.ssid = store_.creds().ssid;
    s.from_nvs = store_.creds().from_nvs;
    s.fast = store_.fast().valid();
    s.ap_ssid = ap_ssid_;
    s.ap_up = f.ap_up;
    s.ap_clients = f.ap_clients;
    s.ap_ip = ap_ip_;
    s.scanning = scan_running_.load();
    if (f.connected) {
        s.ip = WiFi.localIP();
        s.rssi = static_cast<int8_t>(WiFi.RSSI());
    }
    s.channel = radio_channel();
    s.offline_s = f.connected ? 0 : (now - down_since_.load()) / 1000;
    s.failed_attempts = policy_.failed_attempts();
    s.last_reason = last_reason_.load();
    s.pending = pending_set_.load() || pending_forget_.load() || pending_ap_.load() ||
                pending_scan_.load() || pending_ap_off_.load();

    const std::lock_guard<Mutex> g(mtx_);
    status_ = s;
}

// ---------------------------------------------------------------- журнал

void WifiLink::note_down(int reason) {
    const uint32_t now = millis();
    // Причина словами - та же, что видит человек на странице настройки:
    // имя из ядра есть только у ESP32, а разбор общий для обеих плат
    const char *cause = wifi_problem_key(wifi_problem(reason));
    if (!down_) {
        down_ = true;
        down_since_.store(now);
        last_report_ = now;
        LOG_ERROR("wifi: disconnected, " << cause << ", reason " << reason);
    } else if (elapsed(now, last_report_, kReportPeriodMs)) {
        last_report_ = now;
        LOG_ERROR("wifi: offline " << (now - down_since_.load()) / 1000 << " s, "
                                   << policy_.failed_attempts() << " attempts, last "
                                   << cause << " (" << reason << ")");
    }
}

void WifiLink::note_up() {
    const uint32_t now = millis();
    if (down_) {
        LOG_INFO("wifi: back after " << (now - down_since_.load()) / 1000 << " s and "
                                     << policy_.failed_attempts() << " failed attempts");
    }
    down_ = false;
    LOG_INFO("wifi: connected to " << store_.creds().ssid << " via "
                                   << (attempt_fast_ ? "fast" : "scan") << ", ip "
                                   << WiFi.localIP() << ", ch " << WiFi.channel() << ", rssi "
                                   << WiFi.RSSI() << ", gateway " << WiFi.gatewayIP());
}
