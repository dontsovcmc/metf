#include "http_util.h"

namespace http {

const AsyncWebParameter *param_any(AsyncWebServerRequest *request, const char *name) {
    if (request->hasParam(name, true)) return request->getParam(name, true);
    if (request->hasParam(name)) return request->getParam(name);
    return nullptr;
}

void send_400(AsyncWebServerRequest *request, Error err, const String &name) {
    switch (err) {
    case Error::NoGetParam:
        request->send(400, "text/plain", "parameter '" + name + "' not found");
        break;
    case Error::NoFormParam:
        request->send(400, "text/plain", "post form parameter '" + name + "' not found");
        break;
    case Error::IncorrectValue:
        request->send(400, "text/plain", "parameter '" + name + "' is incorrect");
        break;
    }
}

void send_500(AsyncWebServerRequest *request, const String &what) {
    request->send(500, "text/plain", what);
}

} // namespace http
