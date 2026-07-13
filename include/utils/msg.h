//
// Created by fish on 2026/3/25.
//

#pragma once

#if __has_include("bsp/can.h")

#include <cstring>
#include <type_traits>

#include "utils/crc.h"
#include "utils/os.h"
#include "bsp/can.h"
#include "bsp/time.h"

namespace msg {
    template <typename T, bsp_can_e _port, uint32_t _id, int queue_size = 64>
    class can_sender {
    public:
        can_sender(uint8_t pkgs_per_ms = 1)
            : _pkgs_per_ms(pkgs_per_ms), _pkg_queue(queue_size) {
            static_assert(std::is_trivially_copyable_v<T>);
            static_assert(sizeof(T) <= UINT8_MAX);
            static_assert(queue_size > 0);
            BSP_ASSERT(pkgs_per_ms > 0);
        }
        T *operator()() { return &data; }
        void init() {
            BSP_ASSERT(os::task::static_create([&](void *args) {
                auto lst_wkup = bsp_time_get_ms();
                can_pkg_t pkg {};
                bool pending = false;
                for (;;) {
                    uint8_t cur_pkgs = 0;
                    while (cur_pkgs < _pkgs_per_ms) {
                        if (!pending) {
                            if (!_pkg_queue.receive(pkg)) break;
                            pending = true;
                        }
                        if (bsp_can_send(_port, _id, pkg.data, 8) != BSP_STATUS_OK) break;
                        pending = false;
                        cur_pkgs++;
                    }
                    os::task::sleep_until_ms(1, lst_wkup);
                }
            }, nullptr, "can_sender", 128, os::task::Priority::REALTIME));
        }
        bool send() {
            constexpr uint8_t pkg_num = (sizeof(T) + 4 + 8 - 1) / 8;
            static_assert(pkg_num <= queue_size);
            if (bsp_sys_in_isr()) return false;

            can_pkg_t pkg;
            uint8_t tmp[pkg_num * 8];
            memset(tmp, 0, sizeof(tmp));

            // 0xa5 [length] [data] [crc16]
            tmp[0] = 0xa5;
            tmp[1] = sizeof(T);
            memcpy(tmp + 2, &data, sizeof(T));
            const uint16_t crc = crc16::calc(tmp, sizeof(T) + 2, 0xffff);
            memcpy(tmp + 2 + sizeof(T), &crc, sizeof(crc));

            const unsigned long state = bsp_sys_enter_critical();
            if (_pkg_queue.available() < pkg_num) {
                bsp_sys_exit_critical(state);
                return false;
            }
            for (uint8_t i = 0; i < pkg_num; i++) {
                memcpy(pkg.data, tmp + i * 8, 8);
                if (!_pkg_queue.send(pkg)) {
                    bsp_sys_exit_critical(state);
                    return false;
                }
            }
            bsp_sys_exit_critical(state);
            return true;
        }
    private:
        T data {};
        struct can_pkg_t {
            uint8_t data[8];
        } __attribute__((packed));
        uint8_t _pkgs_per_ms;
        os::queue <can_pkg_t> _pkg_queue;
    };

    template <typename T, bsp_can_e _port, uint32_t _id>
    class can_receiver {
    public:
        can_receiver() {
            static_assert(std::is_trivially_copyable_v<T>);
            static_assert(sizeof(T) <= UINT8_MAX);
        }
        void init() {
            self = this;
            bsp_can_set_callback(_port, _id, &can_receiver::callback);
        }
        struct state_t {
            T data {};
            uint32_t timestamp = 0;
        };

        T *operator()() { return &_data; }
        state_t state() const {
            const unsigned long state = bsp_sys_enter_critical();
            const state_t copy = { .data = _data, .timestamp = timestamp };
            bsp_sys_exit_critical(state);
            return copy;
        }
        uint32_t timestamp = 0;
    private:
        T _data {};
        uint8_t buf[((sizeof(T) + 4 + 8 - 1) / 8) * 8] = {};
        size_t ptr = 0;
        static inline can_receiver *self = nullptr;

        static void callback(bsp_can_e port, uint32_t id, const uint8_t *data, size_t len) {
            if (self != nullptr) self->handle(data, len);
        }
        void handle(const uint8_t *data, size_t len) {
            if (data == nullptr || len == 0) return;
            if (ptr + len > sizeof(buf)) {
                ptr = 0;
                return;
            }
            if (ptr == 0) {
                if (len < 2) return;
                if (data[0] != 0xa5 or data[1] != sizeof(T)) return;
                memcpy(buf, data, len); ptr += len;
            } else {
                memcpy(buf + ptr, data, len); ptr += len;
            }
            if (ptr >= sizeof(T) + 4) {
                uint16_t expected_crc;
                memcpy(&expected_crc, buf + sizeof(T) + 2, sizeof(expected_crc));
                if (crc16::calc(buf, sizeof(T) + 2, 0xffff) != expected_crc) {
                    ptr = 0;
                    return;
                }
                T next;
                memcpy(&next, buf + 2, sizeof(T));
                const unsigned long state = bsp_sys_enter_critical();
                _data = next;
                timestamp = bsp_time_get_ms();
                bsp_sys_exit_critical(state);
                ptr = 0;
            }
        }
    };
}

#endif // __has_include("bsp/can.h")
