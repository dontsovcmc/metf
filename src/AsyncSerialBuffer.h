#pragma once
#include <Arduino.h>
#include <cstring>

// Размер кольца задаётся одной константой - сколько байт отдано под лог.
// Число строк производное: длинную строку кольцо всё равно режет на куски по
// ASB_MAX_LINE_LEN, поэтому вмещается не «сто строк», а объём. Значения по
// умолчанию - прежние 100 x 60, они рассчитаны на ESP8266; у ESP32 памяти на
// порядок больше, и объём задаётся флагом в platformio.ini.
//
// Переопределяемо флагами сборки:
//   -DASB_BUFFER_BYTES=...   объём кольца, байт
//   -DASB_MAX_LINE_LEN=...   максимум символов в строке (с нуль-терминатором)
//   -DASB_MAX_LINES=...      если нужно задать число строк напрямую
#ifndef ASB_BUFFER_BYTES
#define ASB_BUFFER_BYTES 6000
#endif
#ifndef ASB_MAX_LINE_LEN
#define ASB_MAX_LINE_LEN 60
#endif
#ifndef ASB_MAX_LINES
#define ASB_MAX_LINES (ASB_BUFFER_BYTES / ASB_MAX_LINE_LEN)
#endif

// Одна ячейка кольца всегда свободна - иначе полное кольцо неотличимо от
// пустого, а на двух строках вытеснение съедало бы весь лог.
static_assert(ASB_MAX_LINES >= 3, "ASB_BUFFER_BYTES слишком мал для ASB_MAX_LINE_LEN");

// Критические секции
#ifdef ARDUINO_ARCH_ESP32
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
extern portMUX_TYPE mux;
#define LOCK()   portENTER_CRITICAL(&mux)
#define UNLOCK() portEXIT_CRITICAL(&mux)
#else
#define LOCK()   noInterrupts()
#define UNLOCK() interrupts()
#endif

class AsyncSerialBuffer {
public:
  AsyncSerialBuffer();

  // Сбросить все накопленные строки и текущую незавершенную
  void flush();

  // Количество готовых строк
  size_t count() const;

  // Сколько строк вытеснено с последнего flush(). Не ноль означает, что лог
  // читали медленнее, чем он приходил, и в нём дыра: молчаливая потеря делает
  // тест ложно-зелёным, поэтому счётчик отдаётся наружу в /read/stat.
  uint32_t dropped() const;

  // Принять байт из входного потока (например, из Serial.read())
  void pushChar(char c);

  // Вывести все накопленные строки в Stream как есть (без добавления символов).
  // После вывода буфер считается пустым.
  void drain_to(Print& out);

private:
  inline size_t inc(size_t x) const { return (x + 1) % ASB_MAX_LINES; }
  inline bool full_unsafe() const   { return inc(head_) == tail_; }
  void push_line();
  void push_line_locked_unchecked();

  // Данные буфера
  char   lines_[ASB_MAX_LINES][ASB_MAX_LINE_LEN]; // готовые строки
  char   current_[ASB_MAX_LINE_LEN];              // накапливаемая строка
  size_t cur_len_;                                // длина текущей строки
  volatile size_t head_;                          // индекс записи
  volatile size_t tail_;                          // индекс чтения
  volatile uint32_t dropped_;                     // вытеснено строк с flush()

  // Нельзя копировать
  AsyncSerialBuffer(const AsyncSerialBuffer&) = delete;
  AsyncSerialBuffer& operator=(const AsyncSerialBuffer&) = delete;
};
