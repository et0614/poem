#include "mcc_generated_files/nvm/nvm.h"
#include "eeprom_manager.h"
#include "utility.h"
#include <util/atomic.h>
#include <string.h>

// AVR128DB32 EEPROM開始アドレス (定数として定義)
#define EEPROM_BASE_ADDR  0x1400

// EEPROM Busy 待ちの上限（反復回数）。EEPROM 1 バイト書き込みは実デバイス仕様で
// 11 ms 程度。F_CPU = 24 MHz、空ループ 1 回あたり数 cycle なので十分な余裕。
#define EEPROM_WAIT_MAX_ITER  200000UL

// EEPROM の完了待ち。タイムアウトしても返すだけで、呼出側には副作用を強要しない。
static bool eeprom_wait_ready(void)
{
    uint32_t iter = 0;
    while (EEPROM_IsBusy())
    {
        if (++iter > EEPROM_WAIT_MAX_ITER) return false;
    }
    return true;
}

// EEPROM全体のマップを定義（型定義のみ）
typedef struct {
    uint8_t init_flag;      // 0x0000
    EM_NetworkSettings net_settings;
    uint8_t is_first_run;
} EepromMap;

// 自動的にアドレス数値に変換
#define ADDR_INIT_FLAG        (EEPROM_BASE_ADDR +offsetof(EepromMap, init_flag))
#define ADDR_NET_SETTING      (EEPROM_BASE_ADDR +offsetof(EepromMap, net_settings))
#define ADDR_IS_FIRST_RUN     (EEPROM_BASE_ADDR +offsetof(EepromMap, is_first_run))

//計測設定
EM_NetworkSettings EM_Network_Settings;
uint8_t EM_Is_First_Run = true;
static EM_NetworkSettings last_network_settings;
static uint8_t last_run_state;

// <editor-fold defaultstate="collapsed" desc="EEPROMブロック読み込み・書き込み処理">

//ブロック書き込み
static void write_eep_block(const void* src, uint16_t dst_addr, size_t size)
{
    const uint8_t* pSrc = (const uint8_t*)src;
    for (size_t i = 0; i < size; i++) {   
        while(EEPROM_IsBusy());
        if (EEPROM_Read(dst_addr + i) != pSrc[i])
        {
            //書き込み可能になるまで待機
            while(EEPROM_IsBusy());
            //アトミックに書き込み実行
            ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
                EEPROM_Write(dst_addr + i, pSrc[i]);
            }
        }
    }
    
    //書き込みが完了したことを確認して処理を終える
    while(EEPROM_IsBusy());
}

// ブロック読み込み
static void read_eep_block(void* dst, uint16_t src_addr, size_t size)
{
    uint8_t* pDst = (uint8_t*)dst;
    for (size_t i = 0; i < size; i++) {
        pDst[i] = EEPROM_Read(src_addr + i);
    }
}

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="初期化処理">

static void initNetwork(){
    EM_Network_Settings = (EM_NetworkSettings){
		1, //バージョン
		{192, 168, 1, 20},     // ip
		{255, 255, 255, 0},     // sn
		{192, 168, 1, 20},      // gw
		{8, 8, 8, 8},           // dns
		{192, 168, 1, 10},     // ブローカーのip
		"my_device",				// トピック名
		0		//CRC（一旦0で初期化）
	};
}

//EEPROMを初期化する
void EM_resetEEPROM()
{
    //初回起動フラグ
    if (eeprom_wait_ready())
        EEPROM_Write(ADDR_IS_FIRST_RUN, 1); //有効

    if (eeprom_wait_ready())
        EEPROM_Write(ADDR_INIT_FLAG, 'T'); //初期化完了
}

//ネットワーク設定をデフォルト値に戻す（RAM上のみ。保存はEM_updateEEPROM）
void EM_initNetworkSettings()
{
    initNetwork();
}

// </editor-fold>

//EEPROMを読み込む
void EM_loadEEPROM()
{
    //ビジーチェック
    eeprom_wait_ready();

    //初期化未了の場合は初期化
    if (EEPROM_Read(ADDR_INIT_FLAG) != 'T') EM_resetEEPROM();

    //ネットワーク設定
    eeprom_wait_ready();
    read_eep_block(&last_network_settings, ADDR_NET_SETTING, sizeof(last_network_settings));
    // CRCを検証
	uint8_t expected_crc = last_network_settings.crc;
	uint8_t actual_crc = calc_crc8(
		(uint8_t*)&last_network_settings,
		sizeof(EM_NetworkSettings) - sizeof(last_network_settings.crc)
	);
    // CRCが一致しない場合にはデフォルト値で再初期化
	if (expected_crc != actual_crc) initNetwork();
    else {
        ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
            memcpy(&EM_Network_Settings, &last_network_settings, sizeof(last_network_settings));
        }   
    }

    //初回起動フラグ
    eeprom_wait_ready();
    EM_Is_First_Run = EEPROM_Read(ADDR_IS_FIRST_RUN);
    last_run_state = EM_Is_First_Run;
}

//設定を保存する
void EM_updateEEPROM()
{
    //EEPROM寿命のため、設定変更があった場合のみ、書き込む
    if(memcmp(&last_network_settings, &EM_Network_Settings, sizeof(last_network_settings)) != 0)
    {
        EM_Network_Settings.crc = calc_crc8(
        (uint8_t*)&EM_Network_Settings,
        sizeof(EM_NetworkSettings) - sizeof(EM_Network_Settings.crc)
        );
        // last_network_settings を更新してから書き込む
        memcpy(&last_network_settings, &EM_Network_Settings, sizeof(last_network_settings));
        write_eep_block(&EM_Network_Settings, ADDR_NET_SETTING, sizeof(EM_Network_Settings));
    }

    if(last_run_state != EM_Is_First_Run)
    {
        last_run_state = EM_Is_First_Run;
        if (eeprom_wait_ready())
            EEPROM_Write(ADDR_IS_FIRST_RUN, EM_Is_First_Run);
    }
}

