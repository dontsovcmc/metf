#include "wifi_portal.h"

#ifdef ESP32
#include <WiFi.h>
#else
#include <ESP8266WiFi.h>
#endif

#include "http_util.h"
#include "logging.h"

using State = WifiPolicy::State;

namespace {

// Адреса, которые телефоны и ноутбуки дёргают, чтобы понять, есть ли
// интернет. Ответ-редирект вместо ожидаемого - и система сама открывает
// страницу. Список - из портала Ватериуса, проверенного на живых телефонах.
const char *const kCaptiveProbes[] = {
    "/generate_204",        // Android
    "/gen_204",             // Android
    "/hotspot-detect.html", // Apple
    "/library/test/success.html",
    "/canonical.html", // Firefox
    "/success.txt",    // Firefox
    "/ncsi.txt",       // Windows
    "/connecttest.txt", "/redirect", "/fwlink",
};

const char *state_name(State s) {
    switch (s) {
    case State::Starting:
        return "starting";
    case State::Connecting:
        return "connecting";
    case State::Online:
        return "online";
    case State::Lost:
        return "lost";
    case State::Ap:
        return "ap";
    }
    return "unknown";
}

String html_escape(const String &in) {
    String out;
    out.reserve(in.length() + 8);
    for (const char c : in) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out += c;
        }
    }
    return out;
}

String json_escape(const String &in) {
    String out;
    out.reserve(in.length() + 4);
    for (const char c : in) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<uint8_t>(c) < 0x20) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        } else {
            out += c;
        }
    }
    return out;
}

// Уровень сигнала полосками, как в телефоне
const char *bars(int8_t rssi) {
    if (rssi >= -55) return "▂▄▆█";
    if (rssi >= -67) return "▂▄▆";
    if (rssi >= -78) return "▂▄";
    return "▂";
}

const char kHead[] PROGMEM = R"(<!DOCTYPE html><html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">)";

const char kStyle[] PROGMEM = R"(<style>
body{font-family:system-ui,sans-serif;margin:0 auto;max-width:28rem;padding:1rem;color:#222;background:#fafafa}
h1{font-size:1.3rem}.box{padding:.8rem 1rem;border-radius:.5rem;margin:.8rem 0;background:#eef}
.ok{background:#e6f6e6}.bad{background:#fbe9e9}.ip{font-size:1.6rem;font-weight:600}
label.net{display:flex;justify-content:space-between;padding:.5rem;border-bottom:1px solid #ddd}
input[type=text],input[type=password]{width:100%;box-sizing:border-box;padding:.6rem;margin:.3rem 0 .8rem;font-size:1rem}
button{padding:.7rem 1.2rem;font-size:1rem;margin:.3rem .3rem 0 0}
small{color:#666}
</style></head><body>)";

} // namespace

void WifiPortal::attach(AsyncWebServer &server) {
    server.on("/", HTTP_GET, [this](AsyncWebServerRequest *r) { on_page(r); });
    server.on("/wifi", HTTP_GET, [this](AsyncWebServerRequest *r) { on_status(r); });
    server.on("/wifi", HTTP_POST, [this](AsyncWebServerRequest *r) { on_command(r); });

    for (const char *uri : kCaptiveProbes) {
        server.on(uri, HTTP_ANY, [this](AsyncWebServerRequest *r) {
            if (!handle_not_found(r)) r->send(404, "text/plain", "Not found");
        });
    }
    // Windows без ответа спрашивает его бесконечно
    server.on("/wpad.dat", HTTP_ANY,
              [](AsyncWebServerRequest *r) { r->send(404, "text/plain", "Not found"); });
}

void WifiPortal::loop() {
    const bool ap = link_.ap_active();
    if (ap && !dns_running_) {
        // На любое имя - адрес платы: так телефон попадает на страницу
        dns_running_ = dns_.start(53, "*", WiFi.softAPIP());
    } else if (!ap && dns_running_) {
        dns_.stop();
        dns_running_ = false;
    }
    if (dns_running_) dns_.processNextRequest();
}

bool WifiPortal::from_ap(AsyncWebServerRequest *request) const {
    return link_.ap_active() && request->client()->localIP() == WiFi.softAPIP();
}

void WifiPortal::redirect_home(AsyncWebServerRequest *request) const {
    request->redirect("http://" + WiFi.softAPIP().toString() + "/");
}

bool WifiPortal::handle_not_found(AsyncWebServerRequest *request) {
    if (!from_ap(request)) return false;
    redirect_home(request);
    return true;
}

// ---------------------------------------------------------------- страница

void WifiPortal::on_page(AsyncWebServerRequest *request) {
    link_.portal_activity();
    const WifiLink::Status s = link_.status();

    WifiLink::ScanEntry nets[WifiLink::kScanMax];
    const int n = link_.scan_results(nets, WifiLink::kScanMax);
    const bool busy = s.pending || s.state == State::Connecting || s.state == State::Starting ||
                      link_.scan_running();

    AsyncResponseStream *res = request->beginResponseStream("text/html; charset=utf-8");
    res->print(FPSTR(kHead));
    // Пока плата подключается или ищет сети - страница обновляется сама
    if (busy) res->print(F("<meta http-equiv=\"refresh\" content=\"2\">"));
    res->print(F("<title>METF: сеть</title>"));
    res->print(FPSTR(kStyle));
    res->print(F("<h1>Сеть платы METF</h1>"));

    // --- состояние
    if (s.connected) {
        res->printf("<div class=\"box ok\">В сети <b>%s</b><br>Адрес платы:<div class=\"ip\">%s</div>"
                    "<small>Стенд обращается к плате по этому адресу.</small></div>",
                    html_escape(s.ssid).c_str(), s.ip.toString().c_str());
    } else if (busy && !link_.scan_running()) {
        res->printf("<div class=\"box\">Подключаюсь к <b>%s</b>…</div>",
                    html_escape(s.ssid).c_str());
    } else if (s.ssid.isEmpty()) {
        res->print(F("<div class=\"box bad\">Сеть не задана.</div>"));
    } else {
        res->printf("<div class=\"box bad\">Нет связи с <b>%s</b>. Причина %d, неудачных попыток %u."
                    "<br><small>Проверьте имя сети и пароль.</small></div>",
                    html_escape(s.ssid).c_str(), s.last_reason,
                    static_cast<unsigned>(s.failed_attempts));
    }
    if (s.hw_error)
        res->print(F("<div class=\"box bad\">Ошибка железа: точка доступа или флеш. "
                     "Подробности в консоли USB.</div>"));

    // --- форма
    res->print(F("<form method=\"post\" action=\"/wifi\"><input type=\"hidden\" name=\"ui\" value=\"1\">"
                 "<input type=\"hidden\" name=\"action\" value=\"set\"><h2>Сети рядом</h2>"));
    if (link_.scan_running()) res->print(F("<p><small>Ищу сети…</small></p>"));
    for (int i = 0; i < n; i++) {
        const String name = html_escape(nets[i].ssid);
        res->printf("<label class=\"net\"><span><input type=\"radio\" name=\"ssid\" value=\"%s\"%s> %s%s</span>"
                    "<small>%s к%u</small></label>",
                    name.c_str(), s.ssid == nets[i].ssid ? " checked" : "", name.c_str(),
                    nets[i].open ? " (открытая)" : "", bars(nets[i].rssi), nets[i].channel);
    }
    res->print(F("<p>Другая сеть:<input type=\"text\" name=\"ssid_manual\" maxlength=\"32\" "
                 "autocapitalize=\"none\" autocorrect=\"off\"></p>"
                 "<p>Пароль:<input type=\"password\" name=\"password\" maxlength=\"63\"></p>"
                 "<button type=\"submit\">Подключить</button></form>"));

    res->print(F("<form method=\"post\" action=\"/wifi\"><input type=\"hidden\" name=\"ui\" value=\"1\">"
                 "<button name=\"action\" value=\"scan\">Обновить список</button>"
                 "<button name=\"action\" value=\"forget\">Сеть из прошивки</button></form>"));
    res->printf("<p><small>Точка платы: %s. METF %s.</small></p></body></html>",
                html_escape(s.ap_ssid).c_str(), METF_VERSION);
    request->send(res);
}

// ---------------------------------------------------------------- API

void WifiPortal::on_status(AsyncWebServerRequest *request) {
    const WifiLink::Status s = link_.status();
    const bool sta = (WiFi.getMode() & WIFI_STA) != 0;
    const char *mode = s.ap_up ? (sta ? "ap_sta" : "ap") : "sta";

    String out;
    out.reserve(360);
    out += "{\"state\":\"";
    out += state_name(s.state);
    out += "\",\"mode\":\"";
    out += mode;
    out += "\",\"connected\":";
    out += s.connected ? "true" : "false";
    out += ",\"ssid\":\"";
    out += json_escape(s.ssid);
    out += "\",\"source\":\"";
    out += s.from_nvs ? "saved" : "build";
    out += "\",\"ip\":\"";
    out += s.connected ? s.ip.toString() : String();
    out += "\",\"rssi\":" + String(s.rssi);
    out += ",\"channel\":" + String(s.channel);
    out += ",\"fast\":";
    out += s.fast ? "true" : "false";
    out += ",\"ap_ssid\":\"";
    out += json_escape(s.ap_ssid);
    out += "\",\"ap_clients\":" + String(s.ap_clients);
    out += ",\"offline_s\":" + String(s.offline_s);
    out += ",\"attempts\":" + String(s.failed_attempts);
    out += ",\"last_reason\":" + String(s.last_reason);
    out += ",\"hw_error\":";
    out += s.hw_error ? "true" : "false";
    out += ",\"pending\":";
    out += s.pending ? "true" : "false";
    out += "}";
    request->send(200, "application/json", out);
}

/*
POST /wifi  action=set&ssid=<имя>&password=<пароль>  сменить сеть
            action=forget                            вернуться к сети из прошивки
            action=ap                                поднять свою точку
            action=scan                              обновить список сетей
Поле ui=1 шлёт форма страницы: ответ - редирект обратно на страницу.
*/
void WifiPortal::on_command(AsyncWebServerRequest *request) {
    link_.portal_activity();

    const AsyncWebParameter *action = http::param_any(request, "action");
    if (!action) {
        http::send_400(request, http::Error::NoFormParam, "action");
        return;
    }
    const bool ui = http::param_any(request, "ui") != nullptr;
    const String a = action->value();

    if (a == "set") {
        // Форма: сеть из списка или введённая руками, введённая важнее
        const AsyncWebParameter *manual = http::param_any(request, "ssid_manual");
        const AsyncWebParameter *picked = http::param_any(request, "ssid");
        String ssid = manual ? manual->value() : String();
        ssid.trim();
        if (ssid.isEmpty() && picked) ssid = picked->value();
        const AsyncWebParameter *pass = http::param_any(request, "password");
        const String password = pass ? pass->value() : String();

        if (!WifiLink::valid_ssid(ssid.c_str())) {
            http::send_400(request, http::Error::IncorrectValue, "ssid");
            return;
        }
        if (!WifiLink::valid_pass(password.c_str())) {
            http::send_400(request, http::Error::IncorrectValue, "password");
            return;
        }
        if (!link_.request_set(ssid.c_str(), password.c_str())) {
            request->send(409, "text/plain", "previous command in progress");
            return;
        }
        LOG_INFO("POST /wifi action=set ssid=" << ssid);
    } else if (a == "forget") {
        link_.request_forget();
    } else if (a == "ap") {
        link_.request_ap();
    } else if (a == "scan") {
        link_.request_scan();
    } else {
        http::send_400(request, http::Error::IncorrectValue, "action");
        return;
    }

    if (ui) {
        request->redirect("/");
    } else {
        request->send(202, "text/plain", "accepted");
    }
}
