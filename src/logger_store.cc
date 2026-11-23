#include "logger_store.h"
#include "log_record.h"
#include "bsp/sys.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

#include "bsp/bsp.h"
#include "bsp/flash.h"
#include "bsp/time.h"

namespace logger::detail {
namespace {

constexpr std::size_t record_queue_depth = 16;
constexpr char boot_id_key[] = "log.boot_id";

struct alignas(4) queued_record_t {
    std::array<std::uint8_t, record_size> data;
};

static_assert(sizeof(queued_record_t) == record_size);

StaticQueue_t record_queue_buffer;
QueueHandle_t record_queue;
alignas(StaticQueue_t) std::array<std::uint8_t, record_queue_depth * sizeof(queued_record_t)>
    record_queue_storage;

StaticSemaphore_t state_mutex_buffer;
SemaphoreHandle_t state_mutex;

StaticTask_t storage_task_buffer;
StackType_t storage_task_stack[1024];

bool initialized;
bool storage_ready;
std::uint32_t boot_id;
std::uint32_t next_sequence;

std::uint32_t dropped_records;
bool clearing;
std::uint32_t clear_epoch;
StaticSemaphore_t clear_done_buffer;
SemaphoreHandle_t clear_done;

void write_u16(std::uint8_t *data, std::uint16_t value) {
    data[0] = static_cast<std::uint8_t>(value);
    data[1] = static_cast<std::uint8_t>(value >> 8U);
}

void write_u32(std::uint8_t *data, std::uint32_t value) {
    data[0] = static_cast<std::uint8_t>(value);
    data[1] = static_cast<std::uint8_t>(value >> 8U);
    data[2] = static_cast<std::uint8_t>(value >> 16U);
    data[3] = static_cast<std::uint8_t>(value >> 24U);
}

void state_lock() {
    (void) xSemaphoreTake(state_mutex, portMAX_DELAY);
}

void state_unlock() {
    (void) xSemaphoreGive(state_mutex);
}

bool recover_next_sequence() {
    const auto used = bsp_flash_log_used_size();
    std::size_t end = used;
    alignas(4) std::array<std::uint8_t, record_size * 2> window;
    while (end >= record_crc_offset) {
        const bool tail = end == used;
        // The final CRC and payload can themselves be entirely erased-state bytes.
        const auto prefix = tail ? record_crc_offset : record_header_size;
        if (end < prefix) break;
        const auto size = std::min(tail ? record_size : window.size(), end);
        const auto start = end - size;
        if (bsp_flash_log_read(start, window.data(), size) != BSP_STATUS_OK) return false;
        if (tail) std::fill(window.begin() + size, window.end(), 0xff);
        const std::span<const std::uint8_t> bytes{window.data(), tail ? window.size() : size};
        for (std::size_t pos = size - prefix;; pos -= 4) {
            record_view_t row;
            const auto len = decode_record(bytes.subspan(pos), row);
            if (len && (pos + len <= size || bsp_flash_log_confirm_end(used, pos + len - size))) {
                next_sequence = row.sequence + 1U;
                return true;
            }
            if (pos == 0) break;
        }
        if (start == 0) break;
        // Include records crossing the start of this window in the next read.
        end = start + record_size - 4;
        bsp_iwdg_refresh();
    }
    return true;
}

[[noreturn]] void storage_task(void *) {
    queued_record_t record;
    for (;;) {
        if (xQueueReceive(record_queue, &record, portMAX_DELAY) != pdTRUE) continue;
        const auto size = read_u32(record.data.data() + 8);
        if (size == 0) {
            const std::uint32_t reset_id = 0;
            const bool ok = bsp_flash_log_clean() == BSP_STATUS_OK &&
                bsp_flash_write_ex(boot_id_key, &reset_id, sizeof(reset_id)) == BSP_STATUS_OK;
            state_lock();
            storage_ready = ok;
            if (ok) {
                boot_id = 0;
                next_sequence = 0;
            }
            state_unlock();
            (void) xSemaphoreGive(clear_done);
            continue;
        }
        auto *data = record.data.data();
        const auto crc = crc32(0, data, record_crc_offset);
        write_u32(data + record_crc_offset, crc32(crc, data + record_header_size, read_u16(data + 24)));
        if (bsp_flash_log_write(data, size) != BSP_STATUS_OK) {
            state_lock();
            storage_ready = false;
            if (!clearing) (void) xQueueReset(record_queue);
            state_unlock();
        }
    }
}

}

bool storage_init(bool write_enabled) {
    if (initialized) return storage_ready;

    state_mutex = xSemaphoreCreateMutexStatic(&state_mutex_buffer);
    storage_ready = bsp_flash_status() == BSP_STATUS_OK &&
                    bsp_flash_log_status() == BSP_STATUS_OK;
    if (storage_ready) {
        std::uint32_t previous_boot_id = 0;
        std::size_t saved_size = 0;
        const auto status = bsp_flash_read_ex(
            boot_id_key, &previous_boot_id, sizeof(previous_boot_id), &saved_size
        );
        storage_ready = status == BSP_STATUS_NOT_FOUND ||
            (status == BSP_STATUS_OK && saved_size == sizeof(previous_boot_id));
        if (storage_ready) storage_ready = recover_next_sequence();
        boot_id = previous_boot_id;
        if (storage_ready && write_enabled) {
            boot_id++;
            if (boot_id == 0) boot_id = 1;
            storage_ready =
                bsp_flash_write_ex(boot_id_key, &boot_id, sizeof(boot_id)) == BSP_STATUS_OK;
        }
    }

    if (storage_ready) {
        clear_done = xSemaphoreCreateBinaryStatic(&clear_done_buffer);
        record_queue = xQueueCreateStatic(
            record_queue_depth, sizeof(queued_record_t), record_queue_storage.data(),
            &record_queue_buffer
        );
        BSP_ASSERT(xTaskCreateStatic(
            storage_task, "logger_store", std::size(storage_task_stack), nullptr,
            tskIDLE_PRIORITY + 2U, storage_task_stack, &storage_task_buffer
        ) != nullptr);
    }

    initialized = true;
    return storage_ready;
}

bool storage_append(level_e level, std::string_view message) {
    if (message.empty()) return false;

    queued_record_t record;
    record.data.fill(0xFF);
    const char *task_name = pcTaskGetName(nullptr);
    const std::size_t task_name_size = std::min(
        std::strlen(task_name), static_cast<std::size_t>(configMAX_TASK_NAME_LEN - 1)
    );
    const std::size_t max_message_size = record_size - record_header_size - task_name_size;
    const std::size_t message_size = std::min(message.size(), max_message_size);
    const std::size_t payload_size = task_name_size + message_size;
    const std::size_t total_size = (record_header_size + payload_size + 3U) & ~std::size_t {3U};

    std::memcpy(record.data.data(), "LOG1", 4);
    record.data[4] = record_version;
    record.data[5] = static_cast<std::uint8_t>(level);
    write_u16(record.data.data() + 6, message_size < message.size() ? 1U : 0U);
    write_u32(record.data.data() + 8, static_cast<std::uint32_t>(total_size));
    write_u32(record.data.data() + 20, bsp_time_get_ms());
    write_u16(record.data.data() + 24, static_cast<std::uint16_t>(payload_size));
    record.data[26] = static_cast<std::uint8_t>(task_name_size);
    record.data[27] = 0;
    std::memcpy(record.data.data() + record_header_size, task_name, task_name_size);
    std::memcpy(
        record.data.data() + record_header_size + task_name_size, message.data(), message_size
    );
    state_lock();
    if (!storage_ready || clearing) {
        state_unlock();
        return false;
    }
    write_u32(record.data.data() + 12, next_sequence);
    write_u32(record.data.data() + 16, boot_id);
    const BaseType_t queued = xQueueSend(record_queue, &record, 0);
    if (queued == pdTRUE) ++next_sequence;
    else ++dropped_records;
    state_unlock();
    return queued == pdTRUE;
}

}

logger::flash::result_e logger::flash::get_info(info_t &info) {
    if (bsp_sys_in_isr() || !detail::state_mutex) return result_e::STORAGE_ERROR;
    detail::state_lock();
    if (detail::clearing) {
        detail::state_unlock();
        return result_e::BUSY;
    }
    const auto epoch = detail::clear_epoch;
    info_t snapshot{};
    snapshot.storage_ready = detail::storage_ready;
    snapshot.boot_id = detail::boot_id;
    snapshot.next_sequence = detail::next_sequence;
    snapshot.dropped_records = detail::dropped_records;
    if (detail::record_queue) snapshot.queued_records = uxQueueMessagesWaiting(detail::record_queue);
    detail::state_unlock();
    snapshot.total_size = bsp_flash_log_total_size();
    snapshot.used_size = bsp_flash_log_used_size();
    detail::state_lock();
    const bool changed = epoch != detail::clear_epoch;
    detail::state_unlock();
    if (changed) return result_e::BUSY;
    info = snapshot;
    return result_e::OK;
}

logger::flash::result_e logger::flash::clear() {
    if (bsp_sys_in_isr() || !detail::state_mutex) return result_e::STORAGE_ERROR;
    detail::state_lock();
    if (!detail::storage_ready || detail::clearing) {
        const auto result = detail::clearing ? result_e::BUSY : result_e::STORAGE_ERROR;
        detail::state_unlock();
        return result;
    }
    detail::clearing = true;
    ++detail::clear_epoch;
    (void) xQueueReset(detail::record_queue);
    // A zero-length record orders the erase after any write already taken by the worker.
    detail::queued_record_t erase{};
    (void) xQueueSend(detail::record_queue, &erase, 0);
    detail::state_unlock();
    (void) xSemaphoreTake(detail::clear_done, portMAX_DELAY);
    detail::state_lock();
    detail::clearing = false;
    const auto result = detail::storage_ready ? result_e::OK : result_e::STORAGE_ERROR;
    detail::state_unlock();
    return result;
}
