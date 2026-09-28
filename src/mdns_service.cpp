#include "mdns_service.h"

#include "logging.h"

#ifdef ESP32
#include <ESPmDNS.h>
#elif defined(ESP8266)
#include <ESP8266mDNS.h>
#endif

void MdnsService::begin(const char *host, const char *instance, uint16_t port,
                        const char *version) {
    host_ = host;
    instance_ = instance;
    version_ = version;
    port_ = port;
}

void MdnsService::loop(bool online) {
    if (online != up_) {
        if (online)
            start();
        else
            stop();
        return;
    }
#ifdef ESP8266
    // У LEAmDNS ответы собираются здесь, а не в прерывании
    if (up_) MDNS.update();
#endif
}

void MdnsService::start() {
    if (!MDNS.begin(host_.c_str())) {
        LOG_ERROR("mdns: " << host_ << ".local not announced");
        return;
    }
    // Имя одно на все платы, а в списке служб они должны различаться
    MDNS.setInstanceName(instance_.c_str());
    MDNS.addService("http", "tcp", port_);
    MDNS.addServiceTxt("http", "tcp", "version", version_.c_str());
    up_ = true;
    LOG_INFO("mdns: http://" << host_ << ".local");
}

void MdnsService::stop() {
    MDNS.end();
    up_ = false;
}
