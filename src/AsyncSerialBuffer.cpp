#include "AsyncSerialBuffer.h"

#ifdef ARDUINO_ARCH_ESP32
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
#endif

// Массивы строк намеренно не обнуляются: кольцо пусто (head_ == tail_), и до
// первой записи никто их не читает, а обнуление 64 КБ на старте - это время,
// за которое UART испытуемого успеет прислать данные.
// cppcheck-suppress uninitMemberVar
AsyncSerialBuffer::AsyncSerialBuffer()
  : cur_len_(0), head_(0), tail_(0), dropped_(0), pushed_(0) {}

void AsyncSerialBuffer::flush() {
  LOCK();
  tail_ = head_;
  cur_len_ = 0;
  dropped_ = 0;   // буфер пуст - считать потери заново
  UNLOCK();
}

size_t AsyncSerialBuffer::count() const {
  LOCK();
  size_t h = head_;
  size_t t = tail_;
  UNLOCK();
  return (h >= t) ? (h - t) : (ASB_MAX_LINES - (t - h));
}

uint32_t AsyncSerialBuffer::seq() const {
  // Без блокировки, как и dropped(): одно выровненное 32-битное поле.
  return pushed_;
}

uint32_t AsyncSerialBuffer::dropped() const {
  // Без блокировки, в отличие от count(): там два индекса и они должны быть
  // согласованы, а здесь одно выровненное 32-битное поле - чтение атомарно.
  // LOCK() на ESP32 - это спинлок с запретом прерываний, и брать его ради
  // одного слова значит глушить приём UART на время запроса /read/stat.
  return dropped_;
}

void AsyncSerialBuffer::push_line_locked_unchecked() {
  if (cur_len_ == 0) return;

  // Нуль-терминатор
  current_[(cur_len_ < (ASB_MAX_LINE_LEN - 1)) ? cur_len_ : (ASB_MAX_LINE_LEN - 1)] = '\0';

  // Вытеснить старейшую строку, если кольцо заполнено. Потеря молчаливая, и
  // по логу её не видно: считаем её здесь, наружу отдаёт /read/stat
  if (full_unsafe()) {
    tail_ = inc(tail_);
    dropped_ = dropped_ + 1;   // ++ по volatile в C++20 устарел
  }

  // Скопировать строку в кольцевой буфер и продвинуть head
  strncpy(lines_[head_], current_, ASB_MAX_LINE_LEN);
  head_ = inc(head_);
  pushed_ = pushed_ + 1;     // ++ по volatile в C++20 устарел
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

uint32_t AsyncSerialBuffer::read_to(Print& out, uint32_t acked) {
  // Забыть подтверждённое и снять снимок - одной критической секцией: между
  // двумя UART успевает положить строку, и номера разъедутся.
  LOCK();
  const uint32_t last = pushed_;
  size_t cnt = count_unsafe();
  if (cnt) {
    const uint32_t first = last - static_cast<uint32_t>(cnt) + 1;
    // Сравнение разностью, а не «больше»: номер 32-битный и однажды переполнится
    if (static_cast<int32_t>(acked - first) >= 0) {
      const uint32_t drop = (static_cast<int32_t>(acked - last) >= 0)
                                ? static_cast<uint32_t>(cnt)
                                : (acked - first + 1);
      tail_ = (tail_ + drop) % ASB_MAX_LINES;
      cnt -= drop;
    }
  }
  size_t t = tail_;
  const size_t h = head_;
  UNLOCK();

  // Печать без сдвига хвоста: строки остаются на плате, пока читатель не
  // подтвердит их следующим запросом
  while (t != h) {
    out.print(lines_[t]);
    out.print('\n');
    t = inc(t);
  }
  return last;
}
