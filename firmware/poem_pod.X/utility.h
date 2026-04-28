/* 
 * File:   utility.h
 * Author: etoga
 *
 * Created on April 27, 2026, 2:31 PM
 */

#ifndef UTILITY_H
#define	UTILITY_H

#ifdef	__cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

 /**
 * @brief CRC8計算関数（多項式 0x31）
 * @param data CRC8を作成するuint8_t型配列
 * @param len 配列要素数
 * @return CRC8
 */
uint8_t calc_crc8(const uint8_t *data, uint8_t len);

 /**
 * @brief "192.168.1.20" 形式のIPv4文字列を4byte配列に変換する。
 * @param str  入力文字列 (NUL終端, '+'/'/'/' '/'&' などで途切れていてもよい)
 * @param out  出力先4byte配列
 * @return true=成功、false=フォーマット異常 (out は不変)
 */
bool parse_ipv4(const char* str, uint8_t out[4]);

 /**
 * @brief floatの上下バイトを入れ替えてエンディアンを変換する
 * @param f float型変数
 */
void swap_float(float* f);

 /**
 * @brief FNV-1aでハッシュを計算する
 * @param data ハッシュを作成するuint8_t型配列
 * @param len 配列要素数
 * @return ハッシュ
 */
uint32_t fnv1a_32(const uint8_t* data, size_t len);



#ifdef	__cplusplus
}
#endif

#endif	/* UTILITY_H */

