#ifndef _ESPTESTFRAMEWORK_LOGGING_h
#define _ESPTESTFRAMEWORK_LOGGING_h

#include <Arduino.h>

// Streaming operator for serial print use. 
template<class T> inline Print &operator <<( Print &obj, T arg ) {
	obj.print( arg ); return obj;
}
#define endl "\r\n"

/*
Generate and print the trailing log timestamp.
02:03:04:005 - hour 2, minute 3, second 4, millisecond 5
*/
#define MS_IN_DAY 86400000
#define MS_IN_HOUR 3600000
#define MS_IN_MINUTE 60000
#define MS_IN_SECOND  1000
#define LOG_FORMAT_TIME do \
{ \
    unsigned long logTime = millis(); \
    unsigned char hours = ( logTime % MS_IN_DAY ) / MS_IN_HOUR; \
    unsigned char minutes = ( ( logTime % MS_IN_DAY ) % MS_IN_HOUR ) / MS_IN_MINUTE; \
    unsigned char seconds = ( ( ( logTime % MS_IN_DAY ) % MS_IN_HOUR ) % MS_IN_MINUTE ) / MS_IN_SECOND; \
    unsigned short ms = ( ( ( ( logTime % MS_IN_DAY ) % MS_IN_HOUR ) % MS_IN_MINUTE ) % MS_IN_SECOND ); \
    char logFormattedTime[13]; \
    sprintf( logFormattedTime, "%02u:%02u:%02u:%03u", hours, minutes, seconds, ms ); \
    Serial << logFormattedTime; \
} while (0)

// Default do no logging...
#define LOG_BEGIN(baud) do {} while (0)
#define LOG_END() do {} while (0)
#define LOG_ERROR(content)     do {} while (0)
#define LOG_INFO(content)      do {} while (0)
#define LOG_DEBUG(content)	    do {} while (0)

#define LOGLEVEL -1
#ifdef LOG_LEVEL_ERROR
#undef LOGLEVEL
#define LOGLEVEL 0
#endif
#ifdef LOG_LEVEL_INFO
#undef LOGLEVEL
#define LOGLEVEL 1
#endif
#ifdef LOG_LEVEL_DEBUG
#undef LOGLEVEL
#define LOGLEVEL 2
#endif

// Depending on log level, add code for logging
#if LOGLEVEL >= 0
	#undef LOG_BEGIN
	#define LOG_BEGIN(baud) do { Serial.begin(baud); } while(0)
	#undef LOG_END
	#define LOG_END() do { Serial.flush(); Serial.end(); } while(0)

    #undef LOG_ERROR
    #define LOG_ERROR(content) do { LOG_FORMAT_TIME; Serial << "  ERROR  : " << content << endl; } while(0)
    #if LOGLEVEL >= 1
        #undef LOG_INFO
        #define LOG_INFO(content) do { LOG_FORMAT_TIME; Serial << "  INFO   : " << content << endl; } while(0)
        #if LOGLEVEL >= 2
            #undef LOG_DEBUG
            #define LOG_DEBUG(content) do { LOG_FORMAT_TIME; Serial << "  DEBUG  : " << content << endl; } while(0)
        #endif // LOGLEVEL >= 3
    #endif // LOGLEVEL >= 2
#endif // LOGLEVEL >= 0

#endif

