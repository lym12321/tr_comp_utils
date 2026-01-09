//
// Created by fish on 2026/1/8.
//

#include "utils/terminal.h"
#include "utils/os.h"

#include <cmath>
#include <cstdarg>

#include "task.h"

using namespace terminal;

namespace terminal {
    static const uint8_t TERMINAL_CLEAR_ALL[]      = "\033[2J\033[1H";
    static const uint8_t TERMINAL_CLEAR_LINE[]     = "\033[2K\r";
    static const uint8_t TERMINAL_CLEAR_BEHIND[]   = "\033[K";
    static const uint8_t KEY_RIGHT[]               = "\033[C";
    static const uint8_t KEY_LEFT[]                = "\033[D";
    static const uint8_t KEY_SAVE[]                = "\033[s";
    static const uint8_t KEY_LOAD[]                = "\033[u";
    struct _cmd {
        std::function<void(const std::vector<std::string>&)> func;
        std::string help;
    };
    static bsp_uart_e _port;
    static bool _inited = false;
    static std::string _buf;
    static std::string _mem[TERMINAL_CMD_MEM_SIZE];
    static int mem_ptr = 0, mem_cur_ptr = 0;

    static bool task_running = false, force_stop = false;
    static os::task daemon_task;
    std::pair<std::function<void(std::vector<std::string>)>, std::vector<std::string>> runtime;
}

static std::unordered_map <std::string, _cmd> cmd;

static void fill_buf(const std::string &val) {
    for (size_t i = 0; i < _buf.size(); i++) send(KEY_LEFT, 3);
    send(TERMINAL_CLEAR_BEHIND, 3);
    _buf = val;
    info("%s", _buf.c_str());
}

void terminal::send(const uint8_t* data, size_t len) {
    if (!_inited) return;
    bsp_uart_send_async(_port, data, len);
}

void terminal::info(const char* fmt, ...) {
    if (!_inited) return;
    va_list ap;
    va_start(ap, fmt);
    uint8_t buf[256];
    const int len = vsnprintf((char *) buf, sizeof(buf), fmt, ap);
    va_end(ap);
    BSP_ASSERT(0 < len && len <= static_cast<int>(sizeof(buf)));
    bsp_uart_send_async(_port, buf, len);
}

void show_prompt() {
    info("%s%s@%s:%s%s%s$\e[00m ", TERMINAL_COLOR_GREEN, "fish", "stm32", TERMINAL_COLOR_BLUE, "~", TERMINAL_COLOR_GREEN);
}

void daemon(void *args) {
    for (;;) {
        if (!task_running) {
            os::task::sleep(10);
            continue;
        }
        runtime.first(runtime.second);
        if (force_stop) info("\r\nstopped\r\n"), force_stop = false;
        show_prompt();
        task_running = false;
    }
}

static void solve() {
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

    if (!args.empty()) {
        if(_mem[(mem_cur_ptr - 1) % TERMINAL_CMD_MEM_SIZE] != _buf) {
            _mem[mem_cur_ptr] = _buf;
            mem_cur_ptr = (mem_cur_ptr + 1) % TERMINAL_CMD_MEM_SIZE;
        }
        mem_ptr = 0;
    }

    if (cmd.count(args[0])) {
        task_running = true;
        // cmd[args[0]].func(args);
        runtime = std::make_pair(cmd[args[0]].func, args);
        return;
    }

    info("command not found: %s\r\n", args[0].c_str());
    show_prompt();
}

static void input(char c) {
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
        solve();
        _buf.clear();
        return;
    }
    if (c == 0x7f) {
        if (_buf.empty()) return;
        info("\b \b");
        _buf.pop_back();
        return;
    }
    if (c == '\t') {
        return;
    }
    if (('a' <= c and c <= 'z') or ('0' <= c and c <= '9') or ('A' <= c and c <= 'Z') or c == ' ') {
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

void terminal::init(const bsp_uart_e port, const int baudrate) {
    daemon_task.create(daemon, nullptr, "terminal", 1024, os::task::Priority::MEDIUM);
    _port = port, _inited = true;
    if (~baudrate)
        bsp_uart_set_baudrate(port, baudrate);
    bsp_uart_set_callback(port, [](bsp_uart_e _, const uint8_t *data, size_t len) {
        recv(data, len);
    });

    register_cmd("help", [](const auto &args) {
        for (auto &[k, v] : cmd) {
            if (v.help.empty())
                info("  %-10s");
            else
                info("  %-10s - %s\r\n", k.c_str(), v.help.c_str());
        }
    }, "Get help information for commands");
    register_cmd("clear", [](const auto &args) {
        send(TERMINAL_CLEAR_ALL, sizeof(TERMINAL_CLEAR_ALL));
    }, "Clear the terminal screen");
    register_cmd("man", [](const auto &args) {
        info("what can i say???\r\n");
    }, "what can i say???");

    register_cmd("reboot", [](const auto &args) {
        bsp_sys_reset();
    }, "reboot the system");

    register_cmd("test", [](const auto &args) {
        for (int i = 0; i < 30 and task_running; i++) {
            info("%d\r\n", i);
            os::task::sleep_seconds(1);
        }
    }, "test");
}

void terminal::register_cmd(const std::string& name, const std::function<void(const std::vector<std::string>& args)>& func, const std::string& help) {
    BSP_ASSERT(cmd.count(name) == 0);
    cmd[name] = { func, help };
}
