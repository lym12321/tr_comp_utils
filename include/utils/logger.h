//
// Created by fish on 2025/9/30.
//

#pragma once

#include <cstddef>
#include <cstdint>

#include "bsp/uart.h"

namespace logger {
    enum level_e {
        NONE,
        ERROR,
        WARNING,
        INFO
    };
    void init(bsp_uart_e port, level_e level);
    bool init_flash(level_e level);
    void info(const char *fmt, ...);
    void warn(const char *fmt, ...);
    void error(const char *fmt, ...);
}

namespace logger::flash {
    enum class result_e {
        OK,
        INVALID_ARGUMENT,
        BUSY,
        NO_SESSION,
        STORAGE_ERROR,
    };

    struct info_t {
        bool storage_ready;
        bool export_active;
        std::uint32_t total_size;
        std::uint32_t used_size;
        std::uint32_t snapshot_size;
        std::uint32_t boot_id;
        std::uint32_t next_sequence;
        std::uint32_t queued_records;
    };

    struct export_t {
        std::uint32_t session;
        std::uint32_t size;
    };

    result_e get_info(info_t &info);
    result_e begin_export(export_t &snapshot);
    result_e read_export(
        std::uint32_t session,
        std::size_t offset,
        std::uint8_t *data,
        std::size_t capacity,
        std::size_t &read_size
    );
    result_e end_export(std::uint32_t session);
    result_e clear();
}
