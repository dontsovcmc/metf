#pragma once
#include <Arduino.h>
#include <cstring>

// Переопределяемо флагами сборки: -DASB_MAX_LINES=... -DASB_MAX_LINE_LEN=...
#ifndef ASB_MAX_LINES
#define ASB_MAX_LINES 100
#endif
#ifndef ASB_MAX_LINE_LEN
#define ASB_MAX_LINE_LEN 60
#endif

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

  // Нельзя копировать
  AsyncSerialBuffer(const AsyncSerialBuffer&) = delete;
  AsyncSerialBuffer& operator=(const AsyncSerialBuffer&) = delete;
};
