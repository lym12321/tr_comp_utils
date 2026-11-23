//
// Created by fish on 2025/9/22.
//

#pragma once

#include "bsp/serial.h"

#if __has_include("bsp/rtt.h")
#include "bsp/rtt.h"
#endif

#include <array>
#include <limits>

namespace vofa {
    // Offline/busy output is dropped.
    template <typename... Args> void send(bsp_serial_t port, Args... args) {
        static_assert((sizeof...(Args) + 1) * sizeof(float) <= BSP_SERIAL_BUFFER_SIZE);
        const std::array<float, sizeof...(Args) + 1> buf = {
            static_cast<float>(args)..., std::numeric_limits<float>::infinity()
        };
        (void) bsp_serial_send_async(port, reinterpret_cast<const uint8_t *>(buf.data()), sizeof(buf));
    }

    template <typename... Args> void send(Args... args) {
        send(E_USB_CDC_0, args...);
    }

#if __has_include("bsp/rtt.h")
    // Send to RTT channel 0
    template <typename... Args> void rtt(Args... args) {
        const std::array<float, sizeof...(Args) + 1> buf = {
            static_cast<float>(args)..., std::numeric_limits<float>::infinity()
        };
        bsp_rtt_write(
            reinterpret_cast <const uint8_t *> (buf.data()),
            static_cast <uint32_t> (buf.size() * sizeof(float))
        );
    }
#endif
}
