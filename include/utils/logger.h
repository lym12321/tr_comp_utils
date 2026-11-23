//
// Created by fish on 2025/9/30.
//

#pragma once

#include <cstddef>
#include <cstdint>

#include "bsp/serial.h"

namespace logger {
    enum level_e {
        NONE,
        ERROR,
        WARNING,
        INFO
    };
    // Initialize before producers. Offline/busy output is dropped.
    void init(bsp_serial_t port, level_e level);
    // Call once before logging; NONE enables maintenance only.
    bool init_flash(level_e level);
    void register_terminal_commands();
    // Logging runs in task context.
    void info(const char *fmt, ...);
    void warn(const char *fmt, ...);
    void error(const char *fmt, ...);
}

namespace logger::flash {
    enum class result_e { OK, INVALID_ARGUMENT, BUSY, NO_SESSION, STORAGE_ERROR };

    struct info_t {
        bool storage_ready;
        bool export_active; // Always false; complete snapshots have been retired.
        std::uint32_t total_size;
        std::uint32_t used_size;
        std::uint32_t snapshot_size;
        std::uint32_t boot_id;
        std::uint32_t next_sequence;
        std::uint32_t queued_records;
        std::uint32_t dropped_records;
    };

    result_e get_info(info_t &info);
    // Task context. Returns after the storage task has discarded pending records and erased the log.
    result_e clear();
}
