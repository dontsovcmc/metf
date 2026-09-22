#pragma once

/*
Где лежит сеть платы: SSID, пароль и пара канал/BSSID для быстрого
подключения.

Сохранённое через портал или POST /wifi перекрывает вкомпилированное
(secrets.ini). forget() стирает сохранённое, и плата возвращается к
вкомпилированной сети.

Пара канал/BSSID меняется при каждом переезде роутера, а креды - только
человеком, поэтому пара пишется отдельно и только при изменении: иначе
каждое подключение переписывало бы запись с паролем.

ESP32 - Preferences (NVS, пространство "metf"). ESP8266 - EEPROM
(эмуляция на флеше) со структурой, магическим числом и CRC: пустой или
битый блок читается как «ничего не сохранено».
*/

#include <cstdint>

class WifiStore {
public:
    static constexpr int kSsidMax = 32;
    static constexpr int kPassMax = 63; // WPA2-PSK, парольная фраза

    struct Creds {
        char ssid[kSsidMax + 1] = {};
        char pass[kPassMax + 1] = {};
        bool from_nvs = false; // false - вкомпилированные

        bool empty() const { return ssid[0] == '\0'; }
    };

    struct Fast {
        uint8_t channel = 0;
        uint8_t bssid[6] = {};

        bool valid() const;
        bool operator==(const Fast &o) const;
    };

    // default_* - вкомпилированная сеть; пустая строка - сети нет
    WifiStore(const char *default_ssid, const char *default_pass);

    // Прочитать сохранённое. false - хранилище не открылось (читаем как пустое)
    bool begin();

    const Creds &creds() const { return creds_; }
    const Fast &fast() const { return fast_; }

    // false - записать не удалось; в памяти новые значения всё равно
    bool save_creds(const char *ssid, const char *pass);
    bool forget();
    bool save_fast(const Fast &f); // пишет, только если пара изменилась
    bool clear_fast();

private:
    void use_defaults();
    bool write_all();

    const char *default_ssid_;
    const char *default_pass_;
    Creds creds_;
    Fast fast_;
};
