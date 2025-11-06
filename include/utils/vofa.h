//
// Created by fish on 2025/9/22.
//

#pragma once

#include "bsp/uart.h"

#include <array>

namespace vofa {
    template <typename... Args> void send(bsp_uart_e device, Args... args) {
        union {
            const uint8_t ch[4] = { 0x00, 0x00, 0x80, 0x7f };
            float f;
        } tail;
        std::array <float, sizeof...(Args) + 1> buf = { static_cast <float> (args)..., tail.f };
        bsp_uart_send_async(device, reinterpret_cast <uint8_t *> (buf.begin()), buf.size() * sizeof(float));
    }
}