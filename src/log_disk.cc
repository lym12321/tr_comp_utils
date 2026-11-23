#include "utils/log_disk.h"

#include "bsp/bsp.h"
#include "bsp/flash.h"
#include "log_record.h"
#include "FreeRTOS.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <new>

namespace {
using namespace logger::detail;

constexpr uint32_t block_size = 512;
constexpr uint32_t clusters = 8192;
constexpr uint32_t fat_blocks = ((clusters + 2) * 2 + block_size - 1) / block_size;
constexpr uint32_t root_lba = 1 + 2 * fat_blocks;
constexpr uint32_t data_lba = root_lba + 1;
constexpr uint32_t disk_blocks = data_lba + clusters;
constexpr uint32_t stride = 4096;
constexpr uint32_t max_source = 1024 * 1024;
constexpr std::string_view columns = "boot\tsequence\tms\tlevel\ttruncated\ttask\tmessage\r\n";
constexpr std::string_view error_text = "Log export unavailable. The flash could not be read, its order could not be recovered, or there was insufficient memory.\r\n";
constexpr std::string_view gap_text = "[damaged log data]\r\n";

struct position_t { uint32_t source, text; };

struct export_t {
    position_t checkpoints[max_source / stride];
    size_t count = 0;
    uint32_t source_size = 0, text_size = 0;
    uint32_t window_start = 0, window_len = 0;
    alignas(4) uint8_t window[block_size];
    position_t cursor{};
    // At most 224 escaped payload bytes and 44 bytes of fields and separators.
    char line[4 * (record_size - record_header_size) + 44];
    size_t line_len = 0;

    bool peek(uint32_t pos, std::span<const uint8_t>& bytes) {
        const size_t needed = std::min<size_t>(record_size, source_size - pos);
        if (pos < window_start || pos + needed > window_start + window_len) {
            const uint32_t len = std::min<uint32_t>(sizeof(window), source_size - pos);
            if (bsp_flash_log_read(pos, window, len) != BSP_STATUS_OK) return false;
            window_start = pos;
            window_len = len;
            bsp_iwdg_refresh();
        }
        bytes = {window + (pos - window_start), needed};
        return true;
    }
    bool next(uint32_t& pos);
    bool read(uint32_t offset, uint8_t* dst, size_t len);
};

size_t utf8_len(const uint8_t* p, size_t left) {
    const unsigned a = p[0];
    if (a >= 0x20 && a <= 0x7e) return 1;
    if (a < 0xc2 || a > 0xf4) return 0;
    const size_t len = a < 0xe0 ? 2 : a < 0xf0 ? 3 : 4;
    if (len > left) return 0;
    for (size_t i = 1; i < len; ++i) if ((p[i] & 0xc0) != 0x80) return 0;
    if ((a == 0xe0 && p[1] < 0xa0) || (a == 0xed && p[1] >= 0xa0) ||
        (a == 0xf0 && p[1] < 0x90) || (a == 0xf4 && p[1] >= 0x90) ||
        (a == 0xc2 && p[1] < 0xa0)) return 0;
    return len;
}

char* escape(char* dst, std::string_view text) {
    static constexpr char hex[] = "0123456789ABCDEF";
    while (!text.empty()) {
        const auto* p = reinterpret_cast<const uint8_t*>(text.data());
        const size_t n = utf8_len(p, text.size());
        if (n != 0) {
            if (*p == '\\') *dst++ = '\\';
            std::memcpy(dst, p, n);
            dst += n;
            text.remove_prefix(n);
        } else {
            *dst++ = '\\';
            *dst++ = 'x';
            *dst++ = hex[*p >> 4];
            *dst++ = hex[*p & 15];
            text.remove_prefix(1);
        }
    }
    return dst;
}

// Return one validated row or one marker for a contiguous damaged region.
bool export_t::next(uint32_t& pos) {
    line_len = 0;
    const uint32_t begin = pos;
    bool damaged = false;
    while (pos < source_size) {
        std::span<const uint8_t> bytes;
        if (!peek(pos, bytes)) return false;
        record_view_t row;
        const size_t size = decode_record(bytes, row);
        if (size) {
            if (begin != pos) { damaged = true; break; }
            char* out = line;
            for (auto n : {row.boot_id, row.sequence, row.time_ms}) {
                out = std::to_chars(out, line + sizeof(line), n).ptr;
                *out++ = '\t';
            }
            const std::string_view level = row.level == 1 ? "ERROR" : row.level == 2 ? "WARN" : "INFO";
            std::memcpy(out, level.data(), level.size());
            out += level.size();
            *out++ = '\t';
            *out++ = (read_u16(bytes.data() + 6) & 1U) ? 'T' : '-';
            *out++ = '\t';
            out = escape(out, row.task);
            *out++ = '\t';
            out = escape(out, row.message);
            *out++ = '\r';
            *out++ = '\n';
            line_len = out - line;
            pos += size;
            return true;
        }
        const size_t n = std::min<size_t>(4, bytes.size());
        for (size_t i = 0; i < n; ++i) if (bytes[i] != 0xff) damaged = true;
        pos += n;
    }
    if (damaged) {
        std::memcpy(line, gap_text.data(), gap_text.size());
        line_len = gap_text.size();
    }
    return true;
}

bool export_t::read(uint32_t offset, uint8_t* dst, size_t len) {
    if (offset < columns.size()) {
        const size_t n = std::min<size_t>(len, columns.size() - offset);
        std::memcpy(dst, columns.data() + offset, n);
        offset += n;
        dst += n;
        len -= n;
    }
    if (!len) return true;
    const auto* after = std::upper_bound(checkpoints, checkpoints + count, offset,
        [](uint32_t key, const position_t& p) { return key < p.text; });
    const position_t start = after[-1];
    if (offset < cursor.text || start.text > cursor.text) {
        cursor = start;
        line_len = 0;
    }
    while (len) {
        if (offset >= cursor.text + line_len) {
            cursor.text += line_len;
            if (!next(cursor.source) || line_len == 0) return false;
            continue;
        }
        const size_t skip = offset - cursor.text;
        const size_t n = std::min(len, line_len - skip);
        std::memcpy(dst, line + skip, n);
        dst += n;
        offset += n;
        len -= n;
    }
    return true;
}

void put16(uint8_t* p, uint16_t n) { p[0] = n; p[1] = n >> 8; }
void put32(uint8_t* p, uint32_t n) { put16(p, n); put16(p + 2, n >> 16); }

bsp_status_t read_blocks(void* ctx, uint32_t lba, uint8_t* dst, size_t blocks) {
    if (!dst || lba >= disk_blocks || blocks > disk_blocks - lba) return BSP_STATUS_ERROR;
    auto* log = static_cast<export_t*>(ctx);
    const uint32_t size = log ? log->text_size : error_text.size();
    const uint32_t last_cluster = 1 + (size + block_size - 1) / block_size;
    for (size_t i = 0; i < blocks; ++i, ++lba, dst += block_size) {
        std::memset(dst, 0, block_size);
        if (lba == 0) {
            dst[0] = 0xeb; dst[1] = 0x3c; dst[2] = 0x90;
            std::memcpy(dst + 3, "MSDOS5.0", 8);
            put16(dst + 11, block_size);
            dst[13] = 1; put16(dst + 14, 1); dst[16] = 2;
            put16(dst + 17, 16); put16(dst + 19, disk_blocks);
            dst[21] = 0xf8; put16(dst + 22, fat_blocks);
            put16(dst + 24, 32); put16(dst + 26, 1);
            dst[36] = 0x80; dst[38] = 0x29; put32(dst + 39, 0x54524C47);
            std::memcpy(dst + 43, "TROBOT LOGS", 11);
            std::memcpy(dst + 54, "FAT16   ", 8);
            dst[510] = 0x55; dst[511] = 0xaa;
        } else if (lba < root_lba) {
            const uint32_t base = ((lba - 1) % fat_blocks) * (block_size / 2);
            for (size_t j = 0; j < block_size / 2; ++j) {
                const uint32_t cluster = base + j;
                if (cluster > last_cluster) break;
                const uint16_t value = cluster == 0 ? 0xfff8 :
                    cluster == 1 || cluster == last_cluster ? 0xffff : cluster + 1;
                put16(dst + j * 2, value);
            }
        } else if (lba == root_lba) {
            std::memcpy(dst, "TROBOT LOGS", 11);
            dst[11] = 0x08;
            uint8_t* entry = dst + 32;
            std::memcpy(entry, log ? "LOG     TXT" : "ERROR   TXT", 11);
            entry[11] = 0x01; put16(entry + 24, 0x21); put16(entry + 26, 2);
            put32(entry + 28, size);
        } else {
            const uint32_t offset = (lba - data_lba) * block_size;
            if (offset >= size) continue;
            const size_t n = std::min<uint32_t>(size - offset, block_size);
            if (log) {
                if (!log->read(offset, dst, n)) return BSP_STATUS_ERROR;
            } else {
                std::memcpy(dst, error_text.data() + offset, n);
            }
        }
    }
    return BSP_STATUS_OK;
}
}

const bsp_usb_disk_t* logger::prepare_log_disk() {
    static bsp_usb_disk_t disk{};
    if (disk.block_count) return &disk;
    disk = {disk_blocks, read_blocks, nullptr};
    size_t size = 0;
    if (bsp_flash_log_open_readonly(&size) != BSP_STATUS_OK || size > max_source) return &disk;
    void* mem = pvPortMalloc(sizeof(export_t));
    if (!mem) return &disk;
    auto* log = new (mem) export_t;
    log->source_size = size;
    log->text_size = columns.size();
    log->checkpoints[log->count++] = {0, log->text_size};
    uint32_t pos = 0;
    while (pos < size) {
        const uint32_t begin = pos;
        if (!log->next(pos)) {
            log->~export_t();
            vPortFree(log);
            return &disk;
        }
        log->text_size += log->line_len;
        if (pos < size && begin / stride != pos / stride) {
            log->checkpoints[log->count++] = {pos, log->text_size};
        }
    }
    log->cursor = log->checkpoints[0];
    log->line_len = 0;
    log->window_len = 0;
    disk.ctx = log;
    return &disk;
}
