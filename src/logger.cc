//
// Created by fish on 2025/9/30.
//

#include "utils/logger.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string_view>

#include "FreeRTOS.h"
#include "task.h"

#include "bsp/sys.h"
#include "logger_store.h"

using namespace logger;

namespace logger {
    static bsp_serial_t _port;
    static bool _port_inited;
    static level_e _level;
    static level_e _flash_level;
    static bool _flash_inited;
}

bool logger::init_flash(level_e level) {
    if (bsp_sys_in_isr() || level < NONE || level > INFO) return false;
    if (_flash_inited) return _flash_level == level;
    if (!detail::storage_init(level != NONE)) return false;
    _flash_level = level;
    _flash_inited = true;
    return true;
}

void logger::init(const bsp_serial_t port, level_e level) {
    BSP_ASSERT(!_port_inited);
    _level = level;
    _port = port;
    _port_inited = true;
}

static void log_va(level_e level, const char *prefix, const char *fmt, va_list ap) {
    if (fmt == nullptr || bsp_sys_in_isr()) return;

    const bool write_flash = _flash_inited && _flash_level >= level;
    const bool write_serial = _port_inited && _level >= level;
    if (!write_flash && !write_serial) return;

    constexpr size_t message_capacity = 256;
    constexpr size_t prefix_capacity = sizeof("[ERROR] <>: ") + configMAX_TASK_NAME_LEN - 1;
    char buf[message_capacity + prefix_capacity - 1];
    size_t pos = 0;
    if (write_serial) {
        const int header_len = std::snprintf(
            buf, prefix_capacity, "[%s] <%s>: ", prefix, pcTaskGetName(nullptr)
        );
        if (header_len < 0) return;
        pos = std::min(static_cast<size_t>(header_len), prefix_capacity - 1);
    }
    const int message_len = std::vsnprintf(buf + pos, message_capacity, fmt, ap);
    if (message_len < 0) return;
    const std::size_t message_size = std::min(
        static_cast<std::size_t>(message_len), message_capacity - 1
    );
    if (write_flash) {
        (void) detail::storage_append(level, std::string_view(buf + pos, message_size));
    }
    if (!write_serial) return;

    pos = std::min(pos + message_size, message_capacity - 2);
    buf[pos++] = '\n';
    (void) bsp_serial_send_async(_port, reinterpret_cast<const uint8_t *>(buf), pos);
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


