#include "utils/logger.h"
#include "utils/terminal.h"
#include "bsp/flash.h"
#include "log_record.h"

void logger::register_terminal_commands() {
    terminal::register_cmd("log", [](const auto &args) {
        if (args.size() != 2 || args[1] != "tail") {
            terminal::info("Usage: log tail\r\n");
            return;
        }
        alignas(4) static std::uint8_t tail[2 * detail::record_size];
        std::size_t size = 0;
        if (bsp_flash_log_tail(tail, sizeof(tail), &size) != BSP_STATUS_OK) {
            terminal::info("Flash log read failed.\r\n");
            return;
        }
        if (size == 0) {
            terminal::info("Flash log is empty.\r\n");
            return;
        }
        const std::span<const std::uint8_t> bytes{tail, size};
        bool found = false;
        for (std::size_t pos = 0; pos + detail::record_header_size <= size && terminal::running();) {
            detail::record_view_t row;
            const auto length = detail::decode_record(bytes.subspan(pos), row);
            if (length == 0) {
                pos += 4;
                continue;
            }
            found = true;
            const char *level = row.level == ERROR ? "ERROR" : row.level == WARNING ? "WARNING" : "INFO";
            terminal::info("boot %lu  %lu ms  %s <%.*s> ", static_cast<unsigned long>(row.boot_id),
                           static_cast<unsigned long>(row.time_ms), level,
                           static_cast<int>(row.task.size()), row.task.data());
            terminal::send(reinterpret_cast<const std::uint8_t *>(row.message.data()), row.message.size());
            terminal::info("\r\n");
            pos += length;
        }
        if (!found && terminal::running()) terminal::info("No complete record in log tail.\r\n");
    }, "Show the last 512 bytes of persisted logs");
}
