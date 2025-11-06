//
// Created by fish on 2025/9/30.
//

#pragma once

#include "bsp/uart.h"

namespace logger {
    enum level_e {
        NONE,
        ERROR,
        WARNING,
        INFO
    };
    void init(bsp_uart_e port, level_e level);
    void info(const char *fmt, ...);
    void warn(const char *fmt, ...);
    void error(const char *fmt, ...);
}
