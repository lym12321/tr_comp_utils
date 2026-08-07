//
// Created by fish on 2025/9/30.
//

#include "utils/logger.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "FreeRTOS.h"
#include "task.h"

#include "bsp/sys.h"
#include "logger_store.h"

using namespace logger;

namespace logger {
    static bsp_uart_e _uart_port;
    static level_e _uart_level;
    static level_e _flash_level;
    static bool _uart_inited;
    static bool _flash_inited;
}

bool logger::init_flash(level_e level) {
    if (bsp_sys_in_isr() || level < NONE || level > INFO) return false;
    if (_flash_inited) return true;
    if (!detail::storage_init(level != NONE)) return false;
    _flash_level = level;
    _flash_inited = true;
    return true;
}

void logger::init(bsp_uart_e port, level_e level) {
    BSP_ASSERT(!_uart_inited);
    _uart_port = port;
    _uart_level = level;
    _uart_inited = true;
}

static void log_va(level_e level, const char *prefix, const char *fmt, va_list ap) {
    if (fmt == nullptr) return;

    const bool in_isr = bsp_sys_in_isr();
    const unsigned long state = bsp_sys_enter_critical();
    const bool write_flash = !in_isr && _flash_inited && _flash_level >= level;
    const bool write_uart = _uart_inited && _uart_level >= level;
    const bsp_uart_e uart_port = _uart_port;
    bsp_sys_exit_critical(state);
    if (!write_flash && !write_uart) return;

    char message[256] = { };
    const int message_len = std::vsnprintf(message, sizeof(message), fmt, ap);
    if (message_len < 0) return;
    const std::size_t message_size = std::min(
        static_cast<std::size_t>(message_len), sizeof(message) - 1
    );
    if (write_flash) {
        (void) detail::storage_append(level, std::string_view(message, message_size));
    }
    if (!write_uart) return;

    char buf[256] = { };
    const char *task_name = in_isr ? "isr" : pcTaskGetName(nullptr);
    const int header_len =
        std::snprintf(buf, sizeof(buf), "[%s] <%s>: ", prefix, task_name);
    if (header_len < 0) return;

    size_t pos = std::min(static_cast<size_t>(header_len), sizeof(buf) - 1);
    const std::size_t copy_size = std::min(message_size, sizeof(buf) - pos - 1);
    std::memcpy(buf + pos, message, copy_size);
    pos += copy_size;

    if (pos < sizeof(buf) - 1) {
        buf[pos++] = '\n';
    } else {
        buf[sizeof(buf) - 2] = '\n';
        pos = sizeof(buf) - 1;
    }

    bsp_uart_send_async(uart_port, reinterpret_cast<const uint8_t*>(buf), pos);
}

void logger::info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_va(INFO, "INFO", fmt, ap);
    va_end(ap);
}

void logger::warn(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_va(WARNING, "WARN", fmt, ap);
    va_end(ap);
}

void logger::error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_va(ERROR, "ERROR", fmt, ap);
    va_end(ap);
}


