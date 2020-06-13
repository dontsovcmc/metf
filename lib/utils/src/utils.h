#include <Arduino.h>

uint8_t hexCharToInt(const char ch);

char intToHexChar(const uint8_t h) ;

bool onlyHexText(const String &str);

size_t hexText2AsciiArray(const String &str, uint8_t *buf, size_t len);