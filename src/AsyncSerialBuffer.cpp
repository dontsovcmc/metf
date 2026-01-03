#include "AsyncSerialBuffer.h"

#ifdef ARDUINO_ARCH_ESP32
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
#endif

AsyncSerialBuffer::AsyncSerialBuffer()
  : cur_len_(0), head_(0), tail_(0) {
  // Опционально обнулить содержимое:
  // memset(lines_, 0, sizeof(lines_));
  // memset(current_, 0, sizeof(current_));
}

void AsyncSerialBuffer::flush() {
  LOCK();
  tail_ = head_;
  cur_len_ = 0;
  UNLOCK();
}

size_t AsyncSerialBuffer::count() const {
  LOCK();
  size_t h = head_;
  size_t t = tail_;
  UNLOCK();
  return (h >= t) ? (h - t) : (ASB_MAX_LINES - (t - h));
}

void AsyncSerialBuffer::push_line_locked_unchecked() {
  if (cur_len_ == 0) return;

  // Нуль-терминатор
  current_[(cur_len_ < (ASB_MAX_LINE_LEN - 1)) ? cur_len_ : (ASB_MAX_LINE_LEN - 1)] = '\0';

  // Вытеснить старейшую строку, если кольцо заполнено
  if (full_unsafe()) {
    tail_ = inc(tail_);
  }

  // Скопировать строку в кольцевой буфер и продвинуть head
  strncpy(lines_[head_], current_, ASB_MAX_LINE_LEN);
  head_ = inc(head_);
  cur_len_ = 0;
}

void AsyncSerialBuffer::push_line() {
  LOCK();
  push_line_locked_unchecked();
  UNLOCK();
}

void AsyncSerialBuffer::pushChar(char c) {
  if (c == '\r') return;

  if (c == '\n') {
    // Завершить и положить строку в буфер
    push_line();
  } else {
    if (cur_len_ < ASB_MAX_LINE_LEN - 1) {
      current_[cur_len_++] = c;
    } else {
      // Переполнение текущей строки — сохранить её и начать новую
      push_line();
      // ВАЖНО: не терять 'c'
      current_[0] = c;
      cur_len_ = 1;
    }
  }
}

void AsyncSerialBuffer::drain_to(Print& out) {
  // Снимок индексов вне длительной критической секции
  LOCK();
  size_t t = tail_;
  size_t h = head_;
  UNLOCK();

  // Печать без модификации данных (никаких '\n' не добавляется)
  while (t != h) {
    out.print(lines_[t]);
    out.print('\n');
    t = inc(t);
  }

  // Пометить все выданные строки как прочитанные
  LOCK();
  tail_ = h;
  UNLOCK();
}
