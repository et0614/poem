/*
 * Utilities.cpp
 *
 * Created: 2025/06/16 9:24:16
 *  Author: e.togashi
 */ 

#include "Utilities.h"

#include <stdlib.h>
#include <string.h>

uint8_t Utilities::crc8(uint8_t *ptr, uint8_t len)
{
	uint8_t crc = 0xFF;
	for(int i = 0; i < len; i++) {
		crc ^= *ptr++;
		for(uint8_t bit = 8; bit > 0; --bit) {
			if(crc & 0x80) {
				crc = (crc << 1) ^ 0x31u;
				} else {
				crc = (crc << 1);
			}
		}
	}
	return crc;
}

uint16_t Utilities::crc16(uint8_t *ptr, uint8_t len)
{
	uint16_t crc = 0xFFFF;
	const uint16_t poly = 0x1021;

	for (uint8_t i = 0; i < len; i++) {
		crc ^= (uint16_t)(*ptr++) << 8;
		for (uint8_t j = 0; j < 8; j++) {
			if (crc & 0x8000) {
				crc = (crc << 1) ^ poly;
				} else {
				crc = crc << 1;
			}
		}
	}
	return crc;
}

/**
 * @brief "192.168.1.50"のような文字列をuint8_t[4]配列に変換する
 * @param ip_str IPアドレス文字列
 * @param result_array 結果を格納する配列
 * @return 成功すればtrue, 失敗すればfalse
 */
bool Utilities::parse_ip_string(const char* ip_str, uint8_t* result_array) {
    char buffer[16];
    strncpy(buffer, ip_str, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

    char* part = strtok(buffer, ".");
    for (int i = 0; i < 4; i++) {
        if (part == NULL) return false; // ドットが3つ未満
        result_array[i] = atoi(part);
        part = strtok(NULL, ".");
    }
    return true;
}