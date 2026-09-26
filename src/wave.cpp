#include "wave.h"

namespace wave {

uint8_t level_at(const Line &line, const uint8_t index) {
    return (index % 2 == 0) ? line.value : static_cast<uint8_t>(line.value ? 0 : 1);
}

namespace {

// Длительность линии: от общего старта пачки до отпускания вывода
uint32_t line_ms(const Line &line) {
    uint32_t sum = line.at_ms;
    for (uint8_t i = 0; i < line.count; i++)
        sum += line.edges[i];
    return sum;
}

} // namespace

Error check(const Batch &batch, uint32_t &total_ms) {
    if (batch.count == 0) return Error::NoLines;
    if (batch.count > kMaxLines) return Error::TooManyLines;

    uint32_t total = 0;

    for (uint8_t i = 0; i < batch.count; i++) {
        const Line &line = batch.lines[i];

        if (line.count == 0) return Error::NoEdges;
        if (line.count > kMaxEdges) return Error::TooManyEdges;

        for (uint8_t e = 0; e < line.count; e++)
            if (line.edges[e] < kMinEdgeMs) return Error::ShortEdge;

        // Два участка на одном выводе шли бы двумя таймерами в один регистр:
        // порядок фронтов стал бы гонкой, а не заказом
        for (uint8_t j = 0; j < i; j++)
            if (batch.lines[j].pin == line.pin) return Error::DuplicatePin;

        const uint32_t ms = line_ms(line);
        if (ms > total) total = ms;
    }

    if (total > kMaxBatchMs) return Error::TooLong;

    total_ms = total;
    return Error::None;
}

const char *error_text(const Error err) {
    switch (err) {
        case Error::None: return "ok";
        case Error::NoLines: return "no lines";
        case Error::TooManyLines: return "too many lines";
        case Error::NoEdges: return "line without edges";
        case Error::TooManyEdges: return "too many edges";
        case Error::ShortEdge: return "edge shorter than 1 ms";
        case Error::TooLong: return "batch longer than 30 s";
        case Error::DuplicatePin: return "pin twice in one batch";
    }
    return "unknown";
}

} // namespace wave
