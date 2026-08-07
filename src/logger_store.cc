#include "logger_store.h"

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
#include "bsp/sys.h"
#include "bsp/time.h"

namespace logger::detail {
namespace {

constexpr std::size_t record_size = 256;
constexpr std::size_t record_header_size = 32;
constexpr std::size_t record_crc_offset = 28;
constexpr std::size_t record_queue_depth = 16;
constexpr std::size_t maintenance_queue_depth = 2;
constexpr char boot_id_key[] = "log.boot_id";

constexpr std::uint8_t record_version = 1;

struct alignas(4) queued_record_t {
    std::array<std::uint8_t, record_size> data;
};

enum class maintenance_operation_e {
    GET_INFO,
    BEGIN_EXPORT,
    READ_EXPORT,
    END_EXPORT,
    CLEAR,
};

struct maintenance_request_t {
    maintenance_operation_e operation;
    flash::result_e result { flash::result_e::STORAGE_ERROR };
    StaticSemaphore_t completion_buffer {};
    SemaphoreHandle_t completion { nullptr };
    flash::info_t *info { nullptr };
    flash::export_t *snapshot { nullptr };
    std::uint32_t session { 0 };
    std::size_t offset { 0 };
    std::uint8_t *data { nullptr };
    std::size_t capacity { 0 };
    std::size_t *read_size { nullptr };
};

static_assert(sizeof(queued_record_t) == record_size);

StaticQueue_t record_queue_buffer;
QueueHandle_t record_queue;
alignas(StaticQueue_t) std::array<std::uint8_t, record_queue_depth * sizeof(queued_record_t)>
    record_queue_storage;

StaticQueue_t maintenance_queue_buffer;
QueueHandle_t maintenance_queue;
alignas(StaticQueue_t) std::array<
    std::uint8_t, maintenance_queue_depth * sizeof(maintenance_request_t *)
> maintenance_queue_storage;

StaticSemaphore_t state_mutex_buffer;
SemaphoreHandle_t state_mutex;

StaticTask_t storage_task_buffer;
StackType_t storage_task_stack[1024];
TaskHandle_t storage_task_handle;

bool initialized;
bool storage_ready;
bool export_active;
bool clearing;
std::uint32_t boot_id;
std::uint32_t next_sequence;
std::uint32_t session_counter;
std::uint32_t active_session;
std::uint32_t snapshot_size;
TickType_t export_last_activity;

std::uint16_t read_u16(const std::uint8_t *data) {
    return static_cast<std::uint16_t>(data[0]) |
           static_cast<std::uint16_t>(data[1]) << 8U;
}

std::uint32_t read_u32(const std::uint8_t *data) {
    return static_cast<std::uint32_t>(data[0]) |
           static_cast<std::uint32_t>(data[1]) << 8U |
           static_cast<std::uint32_t>(data[2]) << 16U |
           static_cast<std::uint32_t>(data[3]) << 24U;
}

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

std::uint32_t crc32(std::uint32_t crc, const std::uint8_t *data, std::size_t size) {
    crc = ~crc;
    while (size-- > 0) {
        crc ^= *data++;
        for (std::uint8_t bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

bool valid_level(level_e level) {
    return level == ERROR || level == WARNING || level == INFO;
}

void state_lock() {
    (void) xSemaphoreTake(state_mutex, portMAX_DELAY);
}

void state_unlock() {
    (void) xSemaphoreGive(state_mutex);
}

bool read_valid_record(
    std::size_t offset,
    std::size_t used_size,
    queued_record_t &record,
    std::uint32_t &sequence,
    std::size_t &total_size
) {
    if (offset + record_header_size > used_size ||
        bsp_flash_log_read(offset, record.data.data(), record_header_size) != BSP_STATUS_OK) {
        return false;
    }
    if (std::memcmp(record.data.data(), "LOG1", 4) != 0 ||
        record.data[4] != record_version) {
        return false;
    }

    total_size = read_u32(record.data.data() + 8);
    const std::size_t payload_size = read_u16(record.data.data() + 24);
    const std::size_t task_name_size = record.data[26];
    if (total_size < record_header_size || total_size > record_size || total_size % 4U != 0U ||
        offset + total_size > used_size || payload_size > total_size - record_header_size ||
        task_name_size > payload_size) {
        return false;
    }
    if (bsp_flash_log_read(offset, record.data.data(), total_size) != BSP_STATUS_OK) {
        return false;
    }

    std::uint32_t expected_crc = crc32(0, record.data.data(), record_crc_offset);
    expected_crc = crc32(
        expected_crc, record.data.data() + record_header_size, payload_size
    );
    if (read_u32(record.data.data() + record_crc_offset) != expected_crc) return false;

    sequence = read_u32(record.data.data() + 12);
    return true;
}

std::uint32_t recover_next_sequence() {
    const std::size_t used_size = bsp_flash_log_used_size();
    std::size_t search_end = used_size;
    queued_record_t window {};
    queued_record_t record {};

    while (search_end >= 4U) {
        const std::size_t search_start = search_end > record_size
            ? search_end - record_size
            : 0;
        const std::size_t read_size = search_end - search_start;
        if (bsp_flash_log_read(
                search_start, window.data.data(), read_size
            ) != BSP_STATUS_OK) {
            storage_ready = false;
            break;
        }

        for (std::size_t position = read_size - 4U;; position -= 4U) {
            if (std::memcmp(window.data.data() + position, "LOG1", 4) == 0) {
                std::uint32_t sequence = 0;
                std::size_t total_size = 0;
                if (read_valid_record(
                        search_start + position, used_size, record, sequence, total_size
                    )) {
                    return sequence + 1U;
                }
            }
            if (position == 0) break;
        }
        if (search_start == 0) break;
        search_end = search_start;
        bsp_iwdg_refresh();
    }
    return 0;
}

bool persist_record(const queued_record_t &record) {
    const std::size_t size = read_u32(record.data.data() + 8);
    if (bsp_flash_log_write(record.data.data(), size) == BSP_STATUS_OK) return true;

    state_lock();
    storage_ready = false;
    state_unlock();
    return false;
}

bool flush_pending_records() {
    queued_record_t record;
    while (xQueueReceive(record_queue, &record, 0) == pdTRUE) {
        if (!persist_record(record)) return false;
    }
    return true;
}

void handle_get_info(maintenance_request_t &request) {
    if (request.info == nullptr) {
        request.result = flash::result_e::INVALID_ARGUMENT;
        return;
    }

    state_lock();
    request.info->storage_ready = storage_ready;
    request.info->export_active = export_active;
    request.info->snapshot_size = snapshot_size;
    request.info->boot_id = boot_id;
    request.info->next_sequence = next_sequence;
    state_unlock();
    request.info->total_size = static_cast<std::uint32_t>(bsp_flash_log_total_size());
    request.info->used_size = static_cast<std::uint32_t>(bsp_flash_log_used_size());
    request.info->queued_records = uxQueueMessagesWaiting(record_queue);
    request.result = flash::result_e::OK;
}

void handle_begin_export(maintenance_request_t &request) {
    if (request.snapshot == nullptr) {
        request.result = flash::result_e::INVALID_ARGUMENT;
        return;
    }

    state_lock();
    if (!storage_ready) {
        state_unlock();
        request.result = flash::result_e::STORAGE_ERROR;
        return;
    }
    if (export_active || clearing) {
        state_unlock();
        request.result = flash::result_e::BUSY;
        return;
    }
    export_active = true;
    active_session = ++session_counter;
    if (active_session == 0) active_session = ++session_counter;
    export_last_activity = xTaskGetTickCount();
    state_unlock();

    if (!flush_pending_records()) {
        state_lock();
        export_active = false;
        active_session = 0;
        snapshot_size = 0;
        state_unlock();
        request.result = flash::result_e::STORAGE_ERROR;
        return;
    }

    state_lock();
    snapshot_size = static_cast<std::uint32_t>(bsp_flash_log_used_size());
    request.snapshot->session = active_session;
    request.snapshot->size = snapshot_size;
    state_unlock();
    request.result = flash::result_e::OK;
}

void handle_read_export(maintenance_request_t &request) {
    if (request.data == nullptr || request.read_size == nullptr || request.capacity == 0 ||
        request.capacity % 4U != 0U || request.offset % 4U != 0U) {
        request.result = flash::result_e::INVALID_ARGUMENT;
        return;
    }

    state_lock();
    const bool valid_session = export_active && request.session == active_session;
    const std::size_t size = snapshot_size;
    if (valid_session) export_last_activity = xTaskGetTickCount();
    state_unlock();
    if (!valid_session) {
        request.result = flash::result_e::NO_SESSION;
        return;
    }
    if (request.offset >= size) {
        request.result = flash::result_e::INVALID_ARGUMENT;
        return;
    }

    *request.read_size = std::min(request.capacity, size - request.offset);
    if (bsp_flash_log_read(request.offset, request.data, *request.read_size) != BSP_STATUS_OK) {
        request.result = flash::result_e::STORAGE_ERROR;
        return;
    }
    request.result = flash::result_e::OK;
}

void handle_end_export(maintenance_request_t &request) {
    state_lock();
    if (!export_active || request.session != active_session) {
        state_unlock();
        request.result = flash::result_e::NO_SESSION;
        return;
    }
    export_active = false;
    active_session = 0;
    snapshot_size = 0;
    state_unlock();
    request.result = flash::result_e::OK;
}

void handle_clear(maintenance_request_t &request) {
    state_lock();
    if (export_active || clearing) {
        state_unlock();
        request.result = flash::result_e::BUSY;
        return;
    }
    if (!storage_ready) {
        state_unlock();
        request.result = flash::result_e::STORAGE_ERROR;
        return;
    }
    clearing = true;
    state_unlock();

    (void) xQueueReset(record_queue);
    bsp_status_t result = bsp_flash_log_clean();
    std::uint32_t reset_boot_id {};
    if (result == BSP_STATUS_OK) {
        result = bsp_flash_write_ex(boot_id_key, &reset_boot_id, sizeof(reset_boot_id));
    }
    state_lock();
    clearing = false;
    snapshot_size = 0;
    if (result == BSP_STATUS_OK) {
        boot_id = reset_boot_id;
        next_sequence = 0;
    } else {
        storage_ready = false;
    }
    state_unlock();
    request.result = result == BSP_STATUS_OK
        ? flash::result_e::OK
        : flash::result_e::STORAGE_ERROR;
}

void handle_maintenance(maintenance_request_t &request) {
    switch (request.operation) {
        case maintenance_operation_e::GET_INFO: handle_get_info(request); break;
        case maintenance_operation_e::BEGIN_EXPORT: handle_begin_export(request); break;
        case maintenance_operation_e::READ_EXPORT: handle_read_export(request); break;
        case maintenance_operation_e::END_EXPORT: handle_end_export(request); break;
        case maintenance_operation_e::CLEAR: handle_clear(request); break;
    }
    (void) xSemaphoreGive(request.completion);
}

void expire_export_if_idle() {
    state_lock();
    if (export_active &&
        xTaskGetTickCount() - export_last_activity >= pdMS_TO_TICKS(5000)) {
        export_active = false;
        active_session = 0;
        snapshot_size = 0;
    }
    state_unlock();
}

[[noreturn]] void storage_task(void *) {
    maintenance_request_t *request;
    queued_record_t record;
    for (;;) {
        bool handled = false;
        while (xQueueReceive(maintenance_queue, &request, 0) == pdTRUE) {
            handle_maintenance(*request);
            handled = true;
        }
        if (xQueueReceive(record_queue, &record, 0) == pdTRUE) {
            (void) persist_record(record);
            handled = true;
        }
        expire_export_if_idle();
        if (!handled) (void) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
    }
}

flash::result_e submit_maintenance(maintenance_request_t &request) {
    if (!initialized || bsp_sys_in_isr()) return flash::result_e::STORAGE_ERROR;

    request.completion = xSemaphoreCreateBinaryStatic(&request.completion_buffer);
    maintenance_request_t *queued_request = &request;
    (void) xQueueSend(maintenance_queue, &queued_request, portMAX_DELAY);
    xTaskNotifyGive(storage_task_handle);
    (void) xSemaphoreTake(request.completion, portMAX_DELAY);
    return request.result;
}

}

bool storage_init(bool advance_boot_id) {
    if (bsp_sys_in_isr()) return false;
    if (initialized) return storage_ready;

    record_queue = xQueueCreateStatic(
        record_queue_depth, sizeof(queued_record_t), record_queue_storage.data(),
        &record_queue_buffer
    );
    maintenance_queue = xQueueCreateStatic(
        maintenance_queue_depth, sizeof(maintenance_request_t *), maintenance_queue_storage.data(),
        &maintenance_queue_buffer
    );
    state_mutex = xSemaphoreCreateMutexStatic(&state_mutex_buffer);

    storage_ready = bsp_flash_status() == BSP_STATUS_OK;
    if (storage_ready) {
        std::uint32_t previous_boot_id = 0;
        std::size_t saved_size = 0;
        (void) bsp_flash_read_ex(
            boot_id_key, &previous_boot_id, sizeof(previous_boot_id), &saved_size
        );
        boot_id = previous_boot_id;
        if (advance_boot_id) {
            boot_id++;
            if (boot_id == 0) boot_id = 1;
            storage_ready =
                bsp_flash_write_ex(boot_id_key, &boot_id, sizeof(boot_id)) == BSP_STATUS_OK;
        }
        if (storage_ready) next_sequence = recover_next_sequence();
    }

    storage_task_handle = xTaskCreateStatic(
        storage_task, "logger_store", std::size(storage_task_stack), nullptr,
        tskIDLE_PRIORITY + 2U, storage_task_stack, &storage_task_buffer
    );

    initialized = true;
    return storage_ready;
}

bool storage_append(level_e level, std::string_view message) {
    if (!initialized || bsp_sys_in_isr() || !valid_level(level) || message.empty()) return false;

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

    state_lock();
    if (!storage_ready || export_active || clearing || uxQueueSpacesAvailable(record_queue) == 0) {
        state_unlock();
        return false;
    }

    std::memcpy(record.data.data(), "LOG1", 4);
    record.data[4] = record_version;
    record.data[5] = static_cast<std::uint8_t>(level);
    write_u16(record.data.data() + 6, message_size < message.size() ? 1U : 0U);
    write_u32(record.data.data() + 8, static_cast<std::uint32_t>(total_size));
    write_u32(record.data.data() + 12, next_sequence++);
    write_u32(record.data.data() + 16, boot_id);
    write_u32(record.data.data() + 20, bsp_time_get_ms());
    write_u16(record.data.data() + 24, static_cast<std::uint16_t>(payload_size));
    record.data[26] = static_cast<std::uint8_t>(task_name_size);
    record.data[27] = 0;
    std::memcpy(record.data.data() + record_header_size, task_name, task_name_size);
    std::memcpy(
        record.data.data() + record_header_size + task_name_size, message.data(), message_size
    );
    std::uint32_t checksum = crc32(0, record.data.data(), record_crc_offset);
    checksum = crc32(checksum, record.data.data() + record_header_size, payload_size);
    write_u32(record.data.data() + record_crc_offset, checksum);

    const BaseType_t queued = xQueueSend(record_queue, &record, 0);
    state_unlock();
    if (queued != pdTRUE) return false;

    xTaskNotifyGive(storage_task_handle);
    return true;
}

}

logger::flash::result_e logger::flash::get_info(info_t &info) {
    detail::maintenance_request_t request {
        .operation = detail::maintenance_operation_e::GET_INFO,
        .info = &info,
    };
    return detail::submit_maintenance(request);
}

logger::flash::result_e logger::flash::begin_export(export_t &snapshot) {
    detail::maintenance_request_t request {
        .operation = detail::maintenance_operation_e::BEGIN_EXPORT,
        .snapshot = &snapshot,
    };
    return detail::submit_maintenance(request);
}

logger::flash::result_e logger::flash::read_export(
    std::uint32_t session,
    std::size_t offset,
    std::uint8_t *data,
    std::size_t capacity,
    std::size_t &read_size
) {
    detail::maintenance_request_t request {
        .operation = detail::maintenance_operation_e::READ_EXPORT,
        .session = session,
        .offset = offset,
        .data = data,
        .capacity = capacity,
        .read_size = &read_size,
    };
    return detail::submit_maintenance(request);
}

logger::flash::result_e logger::flash::end_export(std::uint32_t session) {
    detail::maintenance_request_t request {
        .operation = detail::maintenance_operation_e::END_EXPORT,
        .session = session,
    };
    return detail::submit_maintenance(request);
}

logger::flash::result_e logger::flash::clear() {
    detail::maintenance_request_t request {
        .operation = detail::maintenance_operation_e::CLEAR,
    };
    return detail::submit_maintenance(request);
}
