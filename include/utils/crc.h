//
// Created by fish on 2026/2/3.
//

#pragma once

#include <cstdint>

namespace crc8 {
    static constexpr auto offset = sizeof(uint8_t);

    // 计算 crc8, 默认初始值 0xff
    uint8_t calc(const uint8_t *data, std::size_t size, uint8_t init = 0xff);
    // 计算任意类型数据的 crc8, 不包括末尾的 crc8 字段
    template <typename T>
    uint8_t calc(const T &data, bool offset_last = true) {
        if (offset_last)
            return calc(reinterpret_cast<const uint8_t *>(&data), sizeof(T) - offset);
        return calc(reinterpret_cast<const uint8_t *>(&data), sizeof(T));
    }

    // 验证 crc8 是否正确, 数据末尾 1 Byte 作为 crc8 字段
    inline bool verify(const uint8_t *data, std::size_t size) {
        if(size < offset) return false;
        return *(data + size - offset) == calc(data, size - offset);
    }
    template <typename T>
    bool verify(const T& data) {
        if(sizeof(T) < offset) return false;
        return *(reinterpret_cast <const uint8_t *> (&data) + sizeof(T) - offset) == calc(data);
    }

    // 为数据末尾 1 Byte 的 crc8 字段赋值
    template <typename T>
    void append(T& data) {
        *(reinterpret_cast <uint8_t *> (&data) + sizeof(T) - offset) = calc(data);
    }
}

namespace crc16 {
    static constexpr auto offset = sizeof(uint16_t);

    // 计算 crc16, 默认初始值 0xffff
    uint16_t calc(const uint8_t *data, std::size_t size, uint16_t init = 0xffff);
    // 计算任意类型数据的 crc16, 不包括末尾的 crc16 字段
    template <typename T>
    uint16_t calc(const T& res, bool offset_last = true) {
        if (offset_last)
            return calc(reinterpret_cast <const uint8_t *> (&res), sizeof(T) - offset);
        return calc(reinterpret_cast<const uint8_t *> (&res), sizeof(T));
    }

    // 验证 crc16 是否正确, 数据末尾 2 Byte 作为 crc16 字段
    inline bool verify(const uint8_t *data, std::size_t size) {
        if(size < offset) return false;
        return *reinterpret_cast<const uint16_t *>(data + size - offset) == calc(data, size - offset);
    }
    template <typename T>
    bool verify(const T& data) {
        if(sizeof(T) < offset) return false;
        return *reinterpret_cast <const uint16_t *> (reinterpret_cast <const uint8_t *> (&data) + sizeof(T) - offset) == calc(data);
    }

    // 为数据末尾 2 Byte 的 crc16 字段赋值
    template <typename T>
    void append(T& data) {
        *reinterpret_cast <uint16_t *> (reinterpret_cast <uint8_t *> (&data) + sizeof(T) - offset) = calc(data);
    }
}