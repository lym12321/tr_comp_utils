//
// Created by fish on 2025/9/30.
//

#include "utils/logger.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>

#include "FreeRTOS.h"
#include "task.h"

using namespace logger;

namespace logger {
    static bsp_uart_e _port;
    static level_e _level;
    static bool _inited;
}

void logger::init(bsp_uart_e port, level_e level) {
    BSP_ASSERT(!_inited);
    _port = port;
    _level = level;
    _inited = true;
}

constexpr char header_fmt[] = "[%s] <%s>: ";

static void log_va(const char *prefix, const char *fmt, va_list ap) {
    if (!_inited) return;
    char buf[256] = { };
    int header_len;
    if (xPortIsInsideInterrupt() == pdFALSE)
        header_len = std::snprintf(buf, sizeof(buf), header_fmt, prefix, pcTaskGetName(nullptr));
    else
        header_len = std::snprintf(buf, sizeof(buf), header_fmt, prefix, "isr");

    if (header_len < 0) return;

    size_t pos = std::min(static_cast<size_t>(header_len), sizeof(buf) - 1);
    if (pos < sizeof(buf) - 1) {
        const int body_len = std::vsnprintf(buf + pos, sizeof(buf) - pos, fmt, ap);
        if (body_len < 0) return;
        pos = std::min(pos + static_cast<size_t>(body_len), sizeof(buf) - 1);
    }

    if (pos < sizeof(buf) - 1) {
        buf[pos++] = '\n';
    } else {
        buf[sizeof(buf) - 2] = '\n';
        pos = sizeof(buf) - 1;
    }

    bsp_uart_send_async(_port, reinterpret_cast<const uint8_t*>(buf), pos);
}

void logger::info(const char *fmt, ...) {
    if (_level < INFO) return;
    va_list ap;
    va_start(ap, fmt);
    log_va("INFO", fmt, ap);
    va_end(ap);
}

void logger::warn(const char *fmt, ...) {
    if (_level < WARNING) return;
    va_list ap;
    va_start(ap, fmt);
    log_va("WARN", fmt, ap);
    va_end(ap);
}

void logger::error(const char *fmt, ...) {
    if (_level < ERROR) return;
    va_list ap;
    va_start(ap, fmt);
    log_va("ERROR", fmt, ap);
    va_end(ap);
}


