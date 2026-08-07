//
// Created by fish on 2025/9/22.
//

#pragma once

#include "bsp/uart.h"

#if __has_include("bsp/usb.h")
#include "bsp/usb.h"
#endif

#if __has_include("bsp/rtt.h")
#include "bsp/rtt.h"
#endif

#include <array>

namespace vofa {
    // Send to specified UART device
    template <typename... Args> void send(bsp_uart_e device, Args... args) {
        static_assert((sizeof...(Args) + 1) * sizeof(float) <= BSP_UART_BUFFER_SIZE);
        union {
            const uint8_t ch[4] = { 0x00, 0x00, 0x80, 0x7f };
            float f;
        } tail;
        std::array <float, sizeof...(Args) + 1> buf = { static_cast <float> (args)..., tail.f };
        bsp_uart_send_async(device, reinterpret_cast <uint8_t *> (buf.begin()), buf.size() * sizeof(float));
    }

#if __has_include("bsp/usb.h")
    // Send to USB CDC
    template <typename... Args> void send(Args... args) {
        static_assert((sizeof...(Args) + 1) * sizeof(float) <= BSP_USB_CDC_BUFFER_SIZE);
        union {
            const uint8_t ch[4] = { 0x00, 0x00, 0x80, 0x7f };
            float f;
        } tail;
        std::array <float, sizeof...(Args) + 1> buf = { static_cast <float> (args)..., tail.f };
        bsp_usb_cdc_send(reinterpret_cast <uint8_t *> (buf.begin()), buf.size() * sizeof(float));
    }
#endif

#if __has_include("bsp/rtt.h")
    // Send to RTT channel 0
    template <typename... Args> void rtt(Args... args) {
        union {
            const uint8_t ch[4] = { 0x00, 0x00, 0x80, 0x7f };
            float f;
        } tail;
        std::array <float, sizeof...(Args) + 1> buf = { static_cast <float> (args)..., tail.f };
        bsp_rtt_write(
            reinterpret_cast <const uint8_t *> (buf.data()),
            static_cast <uint32_t> (buf.size() * sizeof(float))
        );
    }
#endif
}
