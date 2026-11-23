//
// Created by fish on 2026/1/8.
//

#include "utils/terminal.h"
#include "utils/os.h"
#include "bsp/uart.h"
#include "bsp/sys.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <unordered_map>

#include "task.h"

using namespace terminal;

namespace terminal {
    static const uint8_t TERMINAL_CLEAR_ALL[]      = "\033[2J\033[1H";
    static const uint8_t TERMINAL_CLEAR_BEHIND[]   = "\033[K";
    struct _cmd {
        std::function<void(const std::vector<std::string>&)> func;
        std::string help;
    };
    static bsp_serial_t _port;
    static bool _inited = false;
    static std::string _buf;
    static std::string _mem[TERMINAL_CMD_MEM_SIZE];
    static int mem_ptr = 0, mem_cur_ptr = 0;

    static std::atomic_bool task_running { false };
    static std::atomic_bool force_stop { false };
    static bool rx_overflow;
    static uint32_t rx_session, edit_session;
    static bool discard_line = false;
    static os::task terminal_task;
    static uint8_t rx_bytes[BSP_SERIAL_BUFFER_SIZE];
    static uint16_t rx_head, rx_tail;
    static_assert((sizeof(rx_bytes) & (sizeof(rx_bytes) - 1)) == 0);
}

static std::unordered_map <std::string, _cmd> cmd;

static void fill_buf(const std::string &val) {
    if (!_buf.empty()) info("\033[%uD", static_cast<unsigned>(_buf.size()));
    send(TERMINAL_CLEAR_BEHIND, 3);
    _buf = val;
    if (!_buf.empty()) send(reinterpret_cast<const uint8_t *>(_buf.data()), _buf.size());
}

bool terminal::running() {
    return task_running.load(std::memory_order_relaxed) && edit_session == bsp_serial_session(_port);
}

bool terminal::cancelled() { return force_stop.load(std::memory_order_relaxed); }

void terminal::send(const uint8_t* data, size_t len) {
    if (!_inited || data == nullptr || len == 0) return;
    if (xTaskGetCurrentTaskHandle() != terminal_task.handle()) {
        (void) bsp_serial_send_async(_port, data, len);
        return;
    }
    while (len > 0) {
        const auto size = std::min(len, static_cast<size_t>(BSP_SERIAL_BUFFER_SIZE));
        const auto lock = bsp_sys_enter_critical();
        const auto status = edit_session == bsp_serial_session(_port) && !cancelled()
            ? bsp_serial_send_async(_port, data, size) : BSP_STATUS_OFFLINE;
        bsp_sys_exit_critical(lock);
        if (status == BSP_STATUS_BUSY) {
            os::task::sleep(1);
            continue;
        }
        if (status != BSP_STATUS_OK) return;
        data += size;
        len -= size;
    }
}

void terminal::info(const char* fmt, ...) {
    if (!_inited) return;
    va_list ap;
    va_start(ap, fmt);
    uint8_t buf[256];
    const int len = vsnprintf((char *) buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len <= 0) return;
    const size_t send_len = std::min(static_cast<size_t>(len), sizeof(buf) - 1);
    send(buf, send_len);
}

void show_prompt() {
    info("%s%s@%s:%s%s%s$\e[00m ", TERMINAL_COLOR_GREEN, "fish", "stm32", TERMINAL_COLOR_BLUE, "~", TERMINAL_COLOR_GREEN);
}

static void solve(bool after_cr) {
    if (_buf.empty()) {
        show_prompt();
        return;
    }
    std::vector <std::string> args;
    std::string cur;
    for (auto &c : _buf) {
        if (c == '\r' or c == '\n') continue;
        if (c == ' ') {
            if (!cur.empty()) {
                args.emplace_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) args.emplace_back(cur);

    if (args.empty()) {
        show_prompt();
        return;
    }

    if (!args.empty()) {
        const int last_idx = (mem_cur_ptr + TERMINAL_CMD_MEM_SIZE - 1) % TERMINAL_CMD_MEM_SIZE;
        if(_mem[last_idx] != _buf) {
            _mem[mem_cur_ptr] = _buf;
            mem_cur_ptr = (mem_cur_ptr + 1) % TERMINAL_CMD_MEM_SIZE;
        }
        mem_ptr = 0;
    }

    vTaskSuspendAll();
    const auto found = cmd.find(args[0]);
    _cmd *command = found == cmd.end() ? nullptr : &found->second;
    (void) xTaskResumeAll();
    if (command != nullptr) {
        const auto lock = bsp_sys_enter_critical();
        const bool current = edit_session == bsp_serial_session(_port);
        task_running = current;
        // Ctrl-C may follow Enter (or CRLF) before this task has started the command.
        uint16_t i = rx_tail;
        if (after_cr && i != rx_head && rx_bytes[i % sizeof(rx_bytes)] == '\n') ++i;
        for (; current && i != rx_head; ++i) {
            const uint8_t c = rx_bytes[i % sizeof(rx_bytes)];
            if (c == '\r' || c == '\n') break;
            if (c == 3) {
                rx_tail = static_cast<uint16_t>(i + 1);
                force_stop = true;
                task_running = false;
                break;
            }
        }
        bsp_sys_exit_critical(lock);
        if (!current) return;
        if (running()) command->func(args);
        const auto finished = bsp_sys_enter_critical();
        task_running = false;
        const bool stopped = force_stop.exchange(false, std::memory_order_relaxed);
        bsp_sys_exit_critical(finished);
        if (stopped) info("\r\nstopped\r\n");
        show_prompt();
        return;
    }

    info("command not found: %s\r\n", args[0].c_str());
    show_prompt();
}

static void input(char c) {
    if (discard_line) {
        if (c == '\r' or c == '\n') {
            discard_line = false;
            info("\r\ninput discarded\r\n");
            show_prompt();
        }
        return;
    }
    if (c == 3) {
        // Ctrl-C
        if (!task_running) return;
        task_running = false;
        force_stop = true;
    }
    if (task_running) return;
    if (c == '\r' or c == '\n') {
        info("\r\n");
        while (!_buf.empty() and _buf.back() == ' ') _buf.pop_back();
        solve(c == '\r');
        _buf.clear();
        return;
    }
    if (c == '\b' or c == 0x7f) {
        if (_buf.empty()) return;
        info("\b \b");
        _buf.pop_back();
        return;
    }
    if (c == '\t') {
        return;
    }
    if (c >= 0x20 and c <= 0x7e) {
        if (_buf.size() >= BSP_SERIAL_BUFFER_SIZE) {
            _buf.clear();
            discard_line = true;
            return;
        }
        send(reinterpret_cast<const uint8_t *>(&c), 1);
        _buf.push_back(c);
    }
}

void recv(const uint8_t *data, size_t len) {
    if (len == 3 and data[0] == 27 and data[1] == 91) {
        if (data[2] == 65) {
            mem_ptr = std::min(mem_ptr + 1,  TERMINAL_CMD_MEM_SIZE);
            if(!_mem[(mem_cur_ptr - mem_ptr + TERMINAL_CMD_MEM_SIZE) % TERMINAL_CMD_MEM_SIZE].empty()) {
                fill_buf(_mem[(mem_cur_ptr - mem_ptr + TERMINAL_CMD_MEM_SIZE) % TERMINAL_CMD_MEM_SIZE]);
            } else {
                mem_ptr --;
            }
        } else if (data[2] == 66) {
            mem_ptr = std::max(mem_ptr - 1, -TERMINAL_CMD_MEM_SIZE);
            if(!_mem[(mem_cur_ptr - mem_ptr + TERMINAL_CMD_MEM_SIZE) % TERMINAL_CMD_MEM_SIZE].empty()) {
                fill_buf(_mem[(mem_cur_ptr - mem_ptr + TERMINAL_CMD_MEM_SIZE) % TERMINAL_CMD_MEM_SIZE]);
            } else {
                mem_ptr ++;
            }
        }
        return;
    }
    for (size_t i = 0; i < len; i++) input(data[i]);
}

static void daemon(void *args) {
    uint8_t esc_buf[3] = { };
    size_t esc_len = 0;

    for (;;) {
        const auto lock = bsp_sys_enter_critical();
        const auto session = bsp_serial_session(_port);
        const bool reconnected = edit_session != session;
        edit_session = session;
        if (rx_session != session) {
            rx_session = session;
            rx_tail = rx_head;
            rx_overflow = false;
        }
        const bool overflow = rx_overflow;
        rx_overflow = false;
        const bool available = rx_head != rx_tail;
        const uint8_t c = available ? rx_bytes[rx_tail++ % sizeof(rx_bytes)] : 0;
        bsp_sys_exit_critical(lock);

        if (reconnected) {
            for (auto &line : _mem) line.clear();
            mem_ptr = mem_cur_ptr = 0;
        }
        if (reconnected || overflow) {
            _buf.clear();
            discard_line = overflow;
            esc_len = 0;
        }
        if (!available) {
            (void) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (discard_line) {
            input(static_cast<char>(c));
            continue;
        }

        if (esc_len > 0) {
            esc_buf[esc_len++] = c;
            if (esc_len == sizeof(esc_buf)) {
                recv(esc_buf, sizeof(esc_buf));
                esc_len = 0;
            }
            continue;
        }

        if (c == 27) {
            esc_buf[0] = c;
            esc_len = 1;
            continue;
        }

        input(static_cast<char>(c));
    }
}

void terminal::init(const bsp_serial_t port, const int baudrate) {
    BSP_ASSERT(!_inited);
    bsp_status_t baud_status = BSP_STATUS_OK;
    if (baudrate != -1 && static_cast<unsigned>(port) < BSP_UART_DEVICE_COUNT) {
        BSP_ASSERT(baudrate > 0);
        baud_status = bsp_uart_set_baudrate(port, static_cast<uint32_t>(baudrate));
    }
    _port = port;
    rx_session = edit_session = bsp_serial_session(port);
    _inited = true;
    BSP_ASSERT(bsp_serial_set_callback(port, [](bsp_serial_t, const uint8_t *data, size_t len) {
        const auto session = bsp_serial_session(_port);
        if (rx_session != session) {
            rx_session = session;
            rx_tail = rx_head;
            rx_overflow = false;
        }
        for (size_t i = 0; i < len; ++i) {
            if (data[i] == 3 && task_running && edit_session == session) {
                force_stop = true;
                task_running = false;
                continue;
            }
            if (static_cast<uint16_t>(rx_head - rx_tail) == sizeof(rx_bytes)) {
                rx_tail = rx_head;
                rx_overflow = true;
            }
            rx_bytes[rx_head++ % sizeof(rx_bytes)] = data[i];
        }
        if (const auto task = terminal_task.handle()) {
            BaseType_t wake = pdFALSE;
            vTaskNotifyGiveFromISR(task, &wake);
            portYIELD_FROM_ISR(wake);
        }
    }) == BSP_STATUS_OK);
    if (baud_status != BSP_STATUS_OK) info("UART baud rate change failed.\r\n");

    register_cmd("help", [](const auto &args) {
        std::vector<const decltype(cmd)::value_type *> entries;
        vTaskSuspendAll();
        entries.reserve(cmd.size());
        for (const auto &entry : cmd) entries.push_back(&entry);
        (void) xTaskResumeAll();
        // Insertions may rehash the table but do not invalidate its entries.
        for (const auto *entry : entries) {
            const auto &[name, command] = *entry;
            if (command.help.empty())
                info("  %-10s\r\n", name.c_str());
            else
                info("  %-10s - %s\r\n", name.c_str(), command.help.c_str());
        }
    }, "Get help information for commands");
    register_cmd("clear", [](const auto &args) {
        send(TERMINAL_CLEAR_ALL, sizeof(TERMINAL_CLEAR_ALL) - 1);
    }, "Clear the terminal screen");
    register_cmd("reboot", [](const auto &args) {
        bsp_sys_reset();
    }, "reboot the system");

    BSP_ASSERT(terminal_task.create(daemon, nullptr, "terminal", 1024, os::task::Priority::MEDIUM));
}

void terminal::register_cmd(const std::string& name, const std::function<void(const std::vector<std::string>& args)>& func, const std::string& help) {
    BSP_ASSERT(_inited);
    vTaskSuspendAll();
    BSP_ASSERT(cmd.emplace(name, _cmd { func, help }).second);
    (void) xTaskResumeAll();
}
