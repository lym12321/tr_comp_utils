//
// Created by fish on 2026/1/8.
//

#pragma once

#include "bsp/uart.h"
#include "bsp/usb.h"

#define TERMINAL_CMD_MEM_SIZE 10

#define ANSI_CSI(code) "\033[" #code "m" /* ANSI CSI指令 */
#define ANSI_BOLD_ON "\x1b[1m"
#define ANSI_BOLD_OFF "\x1b[22m"

/**
 * 终端字体颜色代码
 */
#define TERMINAL_COLOR_BLACK ANSI_CSI(30)     /* 黑色 */
#define TERMINAL_COLOR_RED ANSI_CSI(31)       /* 红色 */
#define TERMINAL_COLOR_GREEN ANSI_CSI(32)     /* 绿色 */
#define TERMINAL_COLOR_YELLOW ANSI_CSI(33)    /* 黄色 */
#define TERMINAL_COLOR_BLUE ANSI_CSI(34)      /* 蓝色 */
#define TERMINAL_COLOR_FUCHSIN ANSI_CSI(35)   /* 品红 */
#define TERMINAL_COLOR_CYAN ANSI_CSI(36)      /* 青色 */
#define TERMINAL_COLOR_WHITE ANSI_CSI(37)     /* 白色 */
#define TERMINAL_COLOR_BLACK_L ANSI_CSI(90)   /* 亮黑 */
#define TERMINAL_COLOR_RED_L ANSI_CSI(91)     /* 亮红 */
#define TERMINAL_COLOR_GREEN_L ANSI_CSI(92)   /* 亮绿 */
#define TERMINAL_COLOR_YELLOW_L ANSI_CSI(93)  /* 亮黄 */
#define TERMINAL_COLOR_BLUE_L ANSI_CSI(94)    /* 亮蓝 */
#define TERMINAL_COLOR_FUCHSIN_L ANSI_CSI(95) /* 亮品红 */
#define TERMINAL_COLOR_CYAN_L ANSI_CSI(96)    /* 亮青 */
#define TERMINAL_COLOR_WHITE_L ANSI_CSI(97)   /* 亮白 */
#define TERMINAL_COLOR_DEFAULT ANSI_CSI(39)   /* 默认 */

#define TERMINAL_SEND(val, sz) bsp_uart_send_async(_port, (uint8_t *)val, sz)
#define TERMINAL_INFO(str, args...) bsp_uart_printf_async(_port, str, ##args)
#define TERMINAL_ERROR(str, args...) bsp_uart_printf_async(_port, TERMINAL_COLOR_RED str, ##args)
#define TERMINAL_ERROR_BLOD(str, args...) bsp_uart_printf_async(_port, ANSI_BOLD_ON TERMINAL_COLOR_RED str ANSI_BOLD_OFF, ##args)

#include <string>
#include <vector>
#include <functional>

namespace terminal {
    void init(bsp_uart_e device, int baudrate = -1);
    void send(const uint8_t *data, size_t len);
    void info(const char *fmt, ...);
    void register_cmd(const std::string &name, const std::function<void(const std::vector<std::string> &argv)> &func, const std::string &help = "");
}