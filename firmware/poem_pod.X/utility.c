#include "utility.h"

// CRC8計算関数（多項式 0x31）
uint8_t calc_crc8(const uint8_t *data, uint8_t len) {
    uint8_t crc = 0xFF;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x80) crc = (crc << 1) ^ 0x31;
            else crc <<= 1;
        }
    }
    return crc;
}

// エンディアン入れ替え関数
void swap_float(float* f) {
    uint8_t* p = (uint8_t*)f;
    uint8_t temp;
    // 0123 -> 3210
    temp = p[0]; p[0] = p[3]; p[3] = temp;
    temp = p[1]; p[1] = p[2]; p[2] = temp;
}

// FNV-1aでハッシュを計算する
uint32_t fnv1a_32(const uint8_t* data, size_t len) {
    uint32_t hash = 2166136261U; // Offset basis
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 16777619U; // FNV prime
    }
    return hash;
}

// IPv4文字列を4byte配列に変換
bool parse_ipv4(const char* str, uint8_t out[4]) {
    uint8_t result[4];
    for (uint8_t octet = 0; octet < 4; octet++) {
        // 数字を読み取り
        if (*str < '0' || *str > '9') return false;
        uint16_t v = 0;
        while (*str >= '0' && *str <= '9') {
            v = v * 10 + (*str - '0');
            if (v > 255) return false;
            str++;
        }
        result[octet] = (uint8_t)v;
        // 区切り (3つ目までは '.', 最後は終端 or 区切り文字いずれでもよい)
        if (octet < 3) {
            if (*str != '.') return false;
            str++;
        }
    }
    out[0] = result[0]; out[1] = result[1]; out[2] = result[2]; out[3] = result[3];
    return true;
}