#pragma once

/*
Общее для обработчиков HTTP: разбор параметров и ответы об ошибках.

Ответы - текстом, как их ждут клиенты стенда: 400 с именем параметра,
500 с описанием отказа железа.
*/

#include <ESPAsyncWebServer.h>

namespace http {

enum class Error : uint8_t {
    NoGetParam,     // нет параметра в строке запроса
    NoFormParam,    // нет поля формы в теле POST
    IncorrectValue  // параметр есть, значение не годится
};

/*
Параметр из тела POST, а если там нет - из строки запроса. `hasParam(name)`
без второго аргумента смотрит только строку запроса, поэтому у /serial и
baudrate, и flush молча не применялись: клиент шлёт их формой в теле.
*/
const AsyncWebParameter *param_any(AsyncWebServerRequest *request, const char *name);

void send_400(AsyncWebServerRequest *request, Error err, const String &name);
void send_500(AsyncWebServerRequest *request, const String &what);

} // namespace http
