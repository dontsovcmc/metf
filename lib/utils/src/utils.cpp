#include "utils.h"

uint8_t hexCharToInt(const char ch)
{
    if (('0' <= ch) && (ch <= '9'))
        return ch - '0';
    if (('A' <= ch) && (ch <= 'F'))
        return 10 + (ch - 'A');
    if (('a' <= ch) && (ch <= 'f'))
        return 10 + (ch - 'a');
    else 
        return 0;
}

char intToHexChar(const uint8_t h) 
{
    if (h <= 9)
        return '0' + h;
    if (h <= 15)
        return 'A' + (h - 10);
    return '0';
}

bool onlyHexText(const String &str)
{
    if (str.length() < 2 || str.length() % 2 == 1) {
        return false;
    }
    
    for (auto ch: str) {
        if ((('0' <= ch) && (ch <= '9'))
         || (('A' <= ch) && (ch <= 'F')) 
         || (('a' <= ch) && (ch <= 'f')))
            continue;
        return false;
    }
    return true;
}

size_t hexText2AsciiArray(const String &in, uint8_t *buf, size_t buf_len)
{   
    if (!onlyHexText(in))
        return 0;
    
    size_t j = 0;
    for(size_t i=0; (i < in.length() - 1) && (j < buf_len); i+=2, j++)
    {
        *(buf+j) = hexCharToInt(in[i]) << 4 | hexCharToInt(in[i+1]);
    }
    
    return j;
}
