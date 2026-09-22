#include "wifi_reason.h"

/*
Коды делятся не по смыслу деаутентификации, а по источнику, и в этом вся
раскладка.

Всё, что меньше 200, - reason code из кадра deauth/disassoc, который прислала
точка доступа (802.11-7.3.1.7). Он говорит одно: связь была и кончилась не по
нашей воле. Почему именно - из него не восстановить: «authentication expired»
шлёт и перезагружающийся роутер, и роутер, выбросивший залежавшуюся станцию.
Единственное исключение - 15: рукопожатие WPA не сошлось, а это авторитетно о
пароле.

Всё, что с 200, - коды самой станции Espressif: их выдаёт машина подключения,
которая знает, что именно не получилось. Им и доверяем поимённо.

Так классификация не требует решать судьбу каждого кода из первой сотни, и
новые коды ядра попадают в осмысленную ветку сами: 210-212 (роутер перевели
на WPA3 или он слишком слаб) - это «сеть не найдена», как и 201.

Нумерация у ESP32 и ESP8266 общая, поэтому ветвления по платформе здесь нет.
Неизвестный код лучше назвать «прочим», чем выдать человеку уверенную
неправду: перенабирать верный пароль он будет долго.
*/
namespace {

constexpr int kEspFirst = 200;      // с этого кода причину сообщает сама станция

constexpr int kHandshake4Way = 15;  // deauth от точки: ключ не сошёлся

constexpr int kBeaconTimeout = 200; // маяки пропали: точку выключили
constexpr int kNoApFound = 201;
constexpr int kAuthFail = 202;
constexpr int kHandshakeTimeout = 204;
constexpr int kNoApCompatibleSecurity = 210;
constexpr int kNoApAuthmodeThreshold = 211;
constexpr int kNoApRssiThreshold = 212;

} // namespace

WifiProblem wifi_problem(int reason) {
    if (reason == 0) return WifiProblem::None;

    if (reason < kEspFirst)
        return reason == kHandshake4Way ? WifiProblem::Password : WifiProblem::Dropped;

    switch (reason) {
    case kAuthFail:
    case kHandshakeTimeout:
        return WifiProblem::Password;

    case kNoApFound:
    case kNoApCompatibleSecurity:
    case kNoApAuthmodeThreshold:
    case kNoApRssiThreshold:
        return WifiProblem::NotFound;

    case kBeaconTimeout:
        return WifiProblem::Dropped;

    default:
        return WifiProblem::Other;
    }
}

/*
Дальше два switch без ветки default: так компилятор (-Wswitch) укажет на
каждое место, где забыли новую причину, а не подсунет человеку слово из
ветки «прочее».
*/
const char *wifi_problem_key(WifiProblem problem) {
    switch (problem) {
    case WifiProblem::None:
        return "none";
    case WifiProblem::Password:
        return "password";
    case WifiProblem::NotFound:
        return "not_found";
    case WifiProblem::Dropped:
        return "dropped";
    case WifiProblem::Other:
        break;
    }
    return "other";
}

const char *wifi_problem_text(WifiProblem problem) {
    switch (problem) {
    case WifiProblem::None:
        return "";
    case WifiProblem::Password:
        return "Неверный пароль.";
    case WifiProblem::NotFound:
        return "Сеть не найдена: проверьте имя, включите роутер или"
               " поднесите плату ближе.";
    case WifiProblem::Dropped:
        return "Роутер разорвал связь: выключен, перезагружается или далеко.";
    case WifiProblem::Other:
        break;
    }
    return "Причина неизвестна, попытки продолжаются.";
}
