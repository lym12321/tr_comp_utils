//
// Created by fish on 2025/9/30.
//

#include "utils/logger.h"
#include <cstring>
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
    static char _buf[256] = { };
    int p;
    if (xPortIsInsideInterrupt() == pdFALSE)
        p = std::snprintf(_buf, sizeof(_buf), header_fmt, prefix, pcTaskGetName(nullptr));
    else
        p = std::snprintf(_buf, sizeof(_buf), header_fmt, prefix, "isr");
    p += std::vsnprintf(_buf + p, sizeof(_buf) - p, fmt, ap);
    _buf[p ++] = '\n';
    bsp_uart_send(_port, reinterpret_cast<const uint8_t*>(_buf), p);
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


