//
// Created by fish on 2026/1/8.
//

#pragma once

#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "queue.h"
#include "task.h"
#include "bsp/sys.h"
#include "bsp/def.h"

#include <utility>
#include <new>

namespace os {
    class task {
    public:
        enum class Priority : UBaseType_t {
            IDLE = osPriorityIdle,
            LOW = osPriorityLow,
            MEDIUM = osPriorityNormal,
            HIGH = osPriorityHigh,
            REALTIME = osPriorityRealtime
        };

        task() = default;
        explicit task(TaskHandle_t h) : handle_(h) {}

        task(const task&) = delete;
        task& operator=(const task&) = delete;

        task(task&& other) noexcept : handle_(other.handle_) {
            other.handle_ = nullptr;
        }
        task& operator=(task&& other) noexcept {
            if (this != &other) {
                handle_ = other.handle_;
                other.handle_ = nullptr;
            }
            return *this;
        }

        template <typename Fn, typename Arg>
        task(Fn&& fn, Arg&& arg, const char *name, uint32_t stack_depth_words, Priority priority) {
            create(std::forward<Fn>(fn), std::forward<Arg>(arg), name, stack_depth_words, priority);
        }

        template <typename Fn, typename Arg>
        bool create(Fn&& fn, Arg&& arg, const char *name, uint32_t stack_depth_words, Priority priority) {
            using FnT = std::decay_t<Fn>;
            using ArgT = std::decay_t<Arg>;

            static_assert(std::is_invocable_r_v<void, FnT, ArgT>, "task expects callable: void fn(arg)");

            struct Thunk {
                FnT fn; ArgT arg;
                static void port(void *p) {
                    auto *self = static_cast <Thunk *> (p);
                    self->fn(self->arg);
                    vPortFree(self);
                    vTaskDelete(nullptr);
                }
            };

            void *mem = pvPortMalloc(sizeof(Thunk));
            if (!mem) {
                handle_ = nullptr;
                return false;
            }

            auto *thunk = new (mem) Thunk { std::forward <Fn> (fn), std::forward <Arg> (arg) };

            BaseType_t ok =
                xTaskCreate(&Thunk::port, name, stack_depth_words, thunk, static_cast <UBaseType_t> (priority), &handle_);
            if (ok != pdPASS) {
                thunk->~Thunk();
                vPortFree(thunk);
                handle_ = nullptr;
                return false;
            }
            return true;
        }

        template <typename Fn, typename Arg>
        static bool static_create(Fn&& fn, Arg&& arg, const char *name, uint32_t stack_depth_words, Priority priority) {
            using FnT = std::decay_t<Fn>;
            using ArgT = std::decay_t<Arg>;

            static_assert(std::is_invocable_r_v<void, FnT, ArgT>, "task expects callable: void fn(arg)");

            struct Thunk {
                FnT fn; ArgT arg;
                static void port(void *p) {
                    auto *self = static_cast <Thunk *> (p);
                    self->fn(self->arg);
                    vPortFree(self);
                    vTaskDelete(nullptr);
                }
            };

            void *mem = pvPortMalloc(sizeof(Thunk));
            if (!mem) {
                return false;
            }

            auto *thunk = new (mem) Thunk { std::forward <Fn> (fn), std::forward <Arg> (arg) };

            BaseType_t ok =
                xTaskCreate(&Thunk::port, name, stack_depth_words, thunk, static_cast <UBaseType_t> (priority), nullptr);
            if (ok != pdPASS) {
                thunk->~Thunk();
                vPortFree(thunk);
                return false;
            }
            return true;
        }

        static void sleep(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
        static void sleep_seconds(uint32_t s) { vTaskDelay(pdMS_TO_TICKS(s * 1000)); }
        static void sleep_until_ms(uint32_t ms, uint32_t &last_weak_up_time) {
            vTaskDelayUntil(&last_weak_up_time, pdMS_TO_TICKS(ms));
        }

        static task current() { return task(xTaskGetCurrentTaskHandle()); }
        void del() {
            if (!handle_) return;
            auto h = handle_;
            handle_ = nullptr;
            vTaskDelete(h);
        }
        void suspend() const { if (handle_) vTaskSuspend(handle_); }
        void resume() const { if (handle_) vTaskResume(handle_); }
        static void yield() { taskYIELD(); }

        [[nodiscard]] TaskHandle_t handle() const { return handle_; }
        explicit operator bool() const { return handle_ != nullptr; }
    private:
        TaskHandle_t handle_ = nullptr;
    };

    class signal {
    public:
        static bool action(task &task, int sig) {
            if (sig < 0 || sig >= 32) return false;
            uint32_t bit = 1u << sig;

            if (bsp_sys_in_isr()) {
                BaseType_t hpw = pdFALSE;
                auto re = xTaskNotifyFromISR(task.handle(), bit, eSetBits, &hpw);
                portYIELD_FROM_ISR(hpw);
                return re == pdPASS;
            } else {
                return xTaskNotify(task.handle(), bit, eSetBits) == pdPASS;
            }
        }

        static bool wait(int sig, uint32_t timeout = UINT32_MAX) {
            if (sig < 0 || sig >= 32) return false;
            const uint32_t bit = 1u << sig;
            const TickType_t start = xTaskGetTickCount();
            TickType_t remaining = timeout;

            for (;;) {
                if ((ulTaskNotifyValueClear(nullptr, bit) & bit) != 0) return true;
                if (remaining == 0) return false;

                uint32_t value = 0;
                if (xTaskNotifyWait(0, bit, &value, remaining) != pdTRUE) return false;
                if ((value & bit) != 0) return true;

                if (timeout == portMAX_DELAY) continue;
                const TickType_t elapsed = xTaskGetTickCount() - start;
                if (elapsed >= timeout) return false;
                remaining = timeout - elapsed;
            }
        }
    };

    template <typename T>
    class queue {
    public:
        explicit queue(unsigned long length) : queue_(xQueueCreate(length, sizeof(T))) {
            BSP_ASSERT(queue_ != nullptr);
        }
        bool send(const T &data) {
            if(bsp_sys_in_isr()) {
                BaseType_t xHigherPriorityTaskWoken = pdFALSE;
                auto re = xQueueSendFromISR(queue_, &data, &xHigherPriorityTaskWoken);
                portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
                return re == pdTRUE;
            } else {
                return xQueueSend(queue_, &data, 0) == pdTRUE;
            }
        }

        bool receive(T &data) {
            if(bsp_sys_in_isr()) {
                BaseType_t xHigherPriorityTaskWoken = pdFALSE;
                auto re = xQueueReceiveFromISR(queue_, &data, &xHigherPriorityTaskWoken);
                portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
                return re == pdTRUE;
            } else {
                return xQueueReceive(queue_, &data, 0) == pdTRUE;
            }
        }

        bool overwrite(T &data) {
            if(bsp_sys_in_isr()) {
                BaseType_t xHigherPriorityTaskWoken = pdFALSE;
                auto re = xQueueOverwriteFromISR(queue_, &data, &xHigherPriorityTaskWoken);
                portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
                return re == pdTRUE;
            } else {
                return xQueueOverwrite(queue_, &data) == pdTRUE;
            }
        }

        [[nodiscard]] bool reset() const { return xQueueReset(queue_) == pdTRUE; }

        [[nodiscard]] unsigned long size() const {
            return bsp_sys_in_isr() ?
                uxQueueMessagesWaitingFromISR(queue_) : uxQueueMessagesWaiting(queue_);
        }

        [[nodiscard]] unsigned long available() const {
            return uxQueueSpacesAvailable(queue_);
        }
    private:
        QueueHandle_t queue_;
    };
}
