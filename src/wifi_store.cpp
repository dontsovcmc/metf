#include "wifi_store.h"

#include <cstring>

#ifdef ESP32
#include <Preferences.h>
#elif defined(ESP8266)
#include <EEPROM.h>
#endif

namespace {

void copy_str(char *dst, size_t size, const char *src) {
    if (src == nullptr) src = "";
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}

} // namespace

bool WifiStore::Fast::valid() const {
    if (channel < 1 || channel > 14) return false;
    // Полпары хуже, чем ничего: быстрое подключение уйдёт в никуда, а скан
    // не начнётся. Нулевой BSSID - не пара.
    for (uint8_t b : bssid)
        if (b != 0) return true;
    return false;
}

bool WifiStore::Fast::operator==(const Fast &o) const {
    return channel == o.channel && memcmp(bssid, o.bssid, sizeof(bssid)) == 0;
}

WifiStore::WifiStore(const char *default_ssid, const char *default_pass)
    : default_ssid_(default_ssid), default_pass_(default_pass) {
    use_defaults();
}

void WifiStore::use_defaults() {
    copy_str(creds_.ssid, sizeof(creds_.ssid), default_ssid_);
    copy_str(creds_.pass, sizeof(creds_.pass), default_pass_);
    creds_.from_nvs = false;
}

bool WifiStore::save_creds(const char *ssid, const char *pass) {
    copy_str(creds_.ssid, sizeof(creds_.ssid), ssid);
    copy_str(creds_.pass, sizeof(creds_.pass), pass);
    creds_.from_nvs = true;
    fast_ = Fast(); // пара принадлежала старой сети
    return write_all();
}

bool WifiStore::forget() {
    use_defaults();
    fast_ = Fast();
    return write_all();
}

bool WifiStore::save_fast(const Fast &f) {
    if (f == fast_) return true;
    fast_ = f;
    return write_all();
}

bool WifiStore::clear_fast() { return save_fast(Fast()); }

// ---------------------------------------------------------------- ESP32: NVS

#ifdef ESP32

namespace {
const char *const kNamespace = "metf";
const char *const kKeySsid = "ssid";
const char *const kKeyPass = "pass";
const char *const kKeyFast = "fast";
} // namespace

bool WifiStore::begin() {
    Preferences p;
    // Только чтение: пространства ещё нет на новой плате, и это не ошибка
    if (!p.begin(kNamespace, true)) return true;

    Creds c;
    if (p.getString(kKeySsid, c.ssid, sizeof(c.ssid)) > 0 && c.ssid[0] != '\0') {
        p.getString(kKeyPass, c.pass, sizeof(c.pass));
        c.from_nvs = true;
        creds_ = c;
    }

    Fast f;
    uint8_t raw[7] = {};
    if (p.getBytes(kKeyFast, raw, sizeof(raw)) == sizeof(raw)) {
        f.channel = raw[0];
        memcpy(f.bssid, raw + 1, sizeof(f.bssid));
        if (f.valid()) fast_ = f;
    }
    p.end();
    return true;
}

bool WifiStore::write_all() {
    Preferences p;
    if (!p.begin(kNamespace, false)) return false;

    bool ok = true;
    if (creds_.from_nvs) {
        ok &= p.putString(kKeySsid, creds_.ssid) == strlen(creds_.ssid);
        ok &= p.putString(kKeyPass, creds_.pass) == strlen(creds_.pass);
    } else {
        p.remove(kKeySsid);
        p.remove(kKeyPass);
    }

    if (fast_.valid()) {
        uint8_t raw[7] = {fast_.channel};
        memcpy(raw + 1, fast_.bssid, sizeof(fast_.bssid));
        ok &= p.putBytes(kKeyFast, raw, sizeof(raw)) == sizeof(raw);
    } else {
        p.remove(kKeyFast);
    }
    p.end();
    return ok;
}

// ---------------------------------------------------------------- ESP8266: EEPROM

#elif defined(ESP8266)

namespace {

constexpr uint32_t kMagic = 0x4D455446; // "METF"

struct Blob {
    uint32_t magic;
    char ssid[WifiStore::kSsidMax + 1];
    char pass[WifiStore::kPassMax + 1];
    uint8_t has_creds;
    uint8_t channel;
    uint8_t bssid[6];
    uint32_t crc;
};

uint32_t crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)));
    }
    return ~crc;
}

uint32_t blob_crc(const Blob &b) {
    return crc32(reinterpret_cast<const uint8_t *>(&b), offsetof(Blob, crc));
}

} // namespace

bool WifiStore::begin() {
    Blob b{};
    EEPROM.begin(sizeof(Blob));
    EEPROM.get(0, b);
    EEPROM.end();

    if (b.magic != kMagic || b.crc != blob_crc(b)) return true; // пусто или битое

    if (b.has_creds && b.ssid[0] != '\0') {
        copy_str(creds_.ssid, sizeof(creds_.ssid), b.ssid);
        copy_str(creds_.pass, sizeof(creds_.pass), b.pass);
        creds_.from_nvs = true;
    }
    Fast f;
    f.channel = b.channel;
    memcpy(f.bssid, b.bssid, sizeof(f.bssid));
    if (f.valid()) fast_ = f;
    return true;
}

bool WifiStore::write_all() {
    Blob b{};
    b.magic = kMagic;
    if (creds_.from_nvs) {
        copy_str(b.ssid, sizeof(b.ssid), creds_.ssid);
        copy_str(b.pass, sizeof(b.pass), creds_.pass);
        b.has_creds = 1;
    }
    if (fast_.valid()) {
        b.channel = fast_.channel;
        memcpy(b.bssid, fast_.bssid, sizeof(b.bssid));
    }
    b.crc = blob_crc(b);

    EEPROM.begin(sizeof(Blob));
    EEPROM.put(0, b);
    const bool ok = EEPROM.commit();
    EEPROM.end();
    return ok;
}

#endif
