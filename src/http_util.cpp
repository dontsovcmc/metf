#include "http_util.h"

namespace http {

const AsyncWebParameter *param_any(AsyncWebServerRequest *request, const char *name) {
    if (request->hasParam(name, true)) return request->getParam(name, true);
    if (request->hasParam(name)) return request->getParam(name);
    return nullptr;
}

void stamp(AsyncWebServerResponse *res) {
    res->addHeader(UPTIME_HEADER, String(millis()));
}

void reply(AsyncWebServerRequest *request, int code, const char *type, const String &body) {
    AsyncWebServerResponse *res = request->beginResponse(code, type, body);
    stamp(res);
    request->send(res);
}

void send_400(AsyncWebServerRequest *request, Error err, const String &name) {
    switch (err) {
    case Error::NoGetParam:
        reply(request, 400, "text/plain", "parameter '" + name + "' not found");
        break;
    case Error::NoFormParam:
        reply(request, 400, "text/plain", "post form parameter '" + name + "' not found");
        break;
    case Error::IncorrectValue:
        reply(request, 400, "text/plain", "parameter '" + name + "' is incorrect");
        break;
    }
}

void send_500(AsyncWebServerRequest *request, const String &what) {
    reply(request, 500, "text/plain", what);
}

} // namespace http
