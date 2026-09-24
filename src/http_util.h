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

/*
Аптайм платы в каждом ответе. По нему стенд узнаёт, что плата перезагрузилась:
уменьшилось - значит был старт заново. Иначе отличить перезагрузку от занятости
неоткуда - `offline_s` в /wifi считается и с потери сети, и с загрузки.

`millis()` переполняется через 49 суток; стенд трактует уменьшение как
перезагрузку, поэтому раз в 49 суток непрерывной работы будет ложная.
*/
constexpr const char *UPTIME_HEADER = "X-Uptime-Ms";

/*
Номер последней строки лога в ответе `/read`. Читатель присылает его обратно
параметром `ack`, и только тогда плата забывает отданное: ответ, не доехавший
по радио, иначе уносит строки навсегда - подтвердить их тем же `200` нельзя, он
приходит позже самой отправки и сам может не дойти.
*/
constexpr const char *LOG_SEQ_HEADER = "X-Log-Seq";

/*
Ответ с аптаймом. Через него идут все обработчики: централизовать нечем -
`DefaultHeaders` хранит только постоянные значения, а middleware веб-сервера
работает до того, как обработчик создаст ответ.
*/
void reply(AsyncWebServerRequest *request, int code, const char *type, const String &body);

/* То же для потоковых ответов, которые обработчик создаёт сам. */
void stamp(AsyncWebServerResponse *res);

void send_400(AsyncWebServerRequest *request, Error err, const String &name);
void send_500(AsyncWebServerRequest *request, const String &what);

} // namespace http
