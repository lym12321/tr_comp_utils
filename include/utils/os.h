//
// Created by fish on 2026/1/8.
//

#pragma once
#include "task.h"
#include "bsp/sys.h"

namespace os {
    class task {
    public:
        enum class Priority : UBaseType_t { IDLE = 0, LOW = 1, MEDIUM = 2, HIGH = 3, REALTIME = 4 };

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
            BSP_ASSERT(0 <= sig and sig < 32);
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
            BSP_ASSERT(0 <= sig and sig < 32);
            uint32_t value = 0;
            auto task = xTaskGetCurrentTaskHandle();
            xTaskNotifyAndQuery(task, 0, eNoAction, &value);

            if(value >> sig & 1) {
                value &= ~(1 << sig);
                xTaskNotify(task, value, eSetValueWithOverwrite);
                return true;
            }

            if(timeout == 0) return false;

            uint32_t current_time = xTaskGetTickCount();
            while(xTaskNotifyWait(0, 1 << sig, &value, timeout) == pdTRUE) {
                if(value >> sig & 1) {
                    return true;
                }

                uint32_t now = xTaskGetTickCount();

                if (now - current_time >= timeout) {
                    return false;
                }

                timeout -= now - current_time;
            }

            return false;
        }
    };
}
