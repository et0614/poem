/*
 * File:   osl_slave.h
 * Author: e.togashi
 *
 * OSL 共通レジスタ v2 の子機側の共通部。全機種で共有する。
 * 仕様は「OSL共通レジスタv2_アドレス表.md」。
 *
 * この層が受け持つもの:
 *   - I2C クライアント (TWI0) の受信・送信と 16bit 番地の解釈、読み出し位置の規則
 *   - 識別・名前・チャネル記述・POLL・拡張区画の組み立てと CRC
 *   - POLL / 拡張ステータスのスナップショット (読み出し中に値が裂けない)
 *   - アドレス変更・名前・ユーザー領域の書き込みと、EEPROM への保存
 *   - コマンド (リセット・識別)、CLKHOLD 固着からの自力復帰、稼働時間
 *
 * 機種側が書くもの:
 *   - osl_config_t (識別情報・チャネル・ユーザー領域) を用意して OSL_Init() を呼ぶ
 *   - 計測のたびに OSL_Publish() を呼ぶ
 *   - メインループで OSL_Task() を、一定周期で OSL_Tick() を呼ぶ
 *
 * 前提: MCC で TWI0 を I2C クライアント (割り込み)、NVM (EEPROM) を生成しておく。
 * プロジェクトのインクルードパスに "." (プロジェクト自身) と "../osl_common" を足す。
 */

#ifndef OSL_SLAVE_H
#define	OSL_SLAVE_H

#ifdef	__cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

// <editor-fold defaultstate="collapsed" desc="定数">

#define OSL_SPEC_VERSION        2

// 1 台あたりのチャネル数の上限 (仕様では 16)。RAM を節約したい機種は、
// このヘッダを読む前に小さい値を #define してよい。
#ifndef OSL_MAX_CHANNELS
#define OSL_MAX_CHANNELS        16
#endif

// 工場出荷時の I2C アドレス (仕様 §3.3)
#define OSL_DEFAULT_I2C_ADDR    0x10

// vendor_id (仕様 §6.1)
#define OSL_VENDOR_POEM         0x0001

// 値の型 (チャネル記述の value_type)
#define OSL_TYPE_FLOAT32        0
#define OSL_TYPE_INT32          1
#define OSL_TYPE_UINT32         2

// 拡張チャネル属性の flags
#define OSL_CHF_COUNTER         0x01    // 積算値 (一巡あり)
#define OSL_CHF_AUX             0x02    // 補助値 (センサ内部温度・生電圧など)

// capability (仕様 §6.3)
#define OSL_CAP_CH_ATTR         0x0001  // 拡張チャネル属性
#define OSL_CAP_EXT_STATUS      0x0002  // 拡張ステータス
#define OSL_CAP_CMD_RESET       0x0004  // リセットコマンド
#define OSL_CAP_CMD_IDENTIFY    0x0008  // 識別コマンド

// status[i] (BACnet Reliability の値、仕様 §6.2)
#define OSL_ST_OK               0       // NO_FAULT_DETECTED
#define OSL_ST_NO_SENSOR        1       // センサ IC が応答しない
#define OSL_ST_OVER_RANGE       2
#define OSL_ST_UNDER_RANGE      3
#define OSL_ST_UNRELIABLE       7       // その他の異常
#define OSL_ST_NOT_READY        64      // (独自) 予熱中・平均窓が未充足

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="機種側が用意する設定">

// チャネル 1 つぶんの記述
typedef struct {
    uint16_t    unit;       // BACnet Engineering Units
    uint8_t     type;       // OSL_TYPE_*
    const char *name;       // チャネル名 (UTF-8、11 バイトまで)
    uint8_t     precision;  // 有効桁数 (0 = 既定の 6 桁)。拡張チャネル属性を実装するときのみ
    uint8_t     flags;      // OSL_CHF_*。拡張チャネル属性を実装するときのみ
} osl_channel_t;

typedef struct {
    // 識別 (仕様 §3.2)
    uint16_t vendor_id;
    uint16_t model_id;
    uint8_t  fw_major, fw_minor, fw_patch;
    uint16_t update_period_ms;

    // チャネル (1〜OSL_MAX_CHANNELS)
    uint8_t              channel_count;
    const osl_channel_t *channels;

    // 拡張識別 (仕様 §4.2)。capability = 0 なら任意区画はすべて未実装になる。
    uint16_t capability;
    uint8_t  hw_rev;
    uint16_t avg_window_ms;
    uint16_t warmup_ms;

    // 工場出荷時の I2C アドレス。0 なら OSL_DEFAULT_I2C_ADDR。
    uint8_t  default_i2c_addr;

    // ユーザー領域 (0x1000〜)。機種固有の設定 (補正係数など) をここに置く。
    // EEPROM への保存はこの層が行う。不要なら size = 0。
    void       *user_region;
    uint16_t    user_size;
    bool        user_writable;
    const void *user_default;   // 初期値 (EEPROM が壊れていたとき、初回起動時)

    // 識別コマンドを受けたときに呼ばれる (割り込みの外、OSL_Task から)。
    // seconds = 0 は停止。OSL_CAP_CMD_IDENTIFY を立てた機種だけ用意する。
    void (*on_identify)(uint8_t seconds);

    // 親機がユーザー領域を書き換えたときに呼ばれる (割り込みの外、OSL_Task から)。
    // 設定の変更に応じた処理 (フィルタの作り直し、ヒーターの入切など) を行う。不要なら NULL。
    void (*on_user_changed)(void);
} osl_config_t;

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="API">

// 初期化。EEPROM を読み、I2C アドレスを反映し、各区画を組み立て、TWI0 の
// コールバックを登録する。cfg は呼び出し後も保持される (static に置くこと)。
void OSL_Init(const osl_config_t *cfg);

// メインループから毎回呼ぶ。EEPROM への保存とコマンドの実行を行う。
// EEPROM の書き込みは 1 バイト約 10 ms かかるので、割り込みの中では行わない。
void OSL_Task(void);

// 一定周期で呼ぶ (経過時間 [ms] を渡す)。稼働時間を進め、TWI0 の CLKHOLD 固着を
// 監視する (1 秒続いたら自分から取引を打ち切る)。
void OSL_Tick(uint16_t elapsed_ms);

// 計測値を公開する。values は 4 バイト × channel_count (型はチャネル記述どおり)、
// status は channel_count バイト。sample_seq を 1 進めて POLL を作り直す。
// values = NULL なら値と sample_seq はそのままで、状態だけを更新する。
void OSL_Publish(const void *values, const uint8_t *status);

// 工場出荷状態に戻す: I2C アドレスを既定値に、名前を空に、ユーザー領域を初期値にして
// EEPROM に保存する。新しいアドレスにはすぐ切り替わる。機種側の操作から呼ぶための
// もので、遠隔からは呼べない。PoEM の子機は工場リセットのスイッチを持たないので呼ばない
// (上位機器がアドレス・名前・ユーザー領域を書き換えて同じ状態にする)。
void OSL_FactoryReset(void);

// センサとの通信の異常を 1 回数える (拡張ステータスの err_sensor)。
void OSL_CountSensorError(void);

// 準備中 (予熱中・平均窓が未充足) かどうか (拡張ステータスの node_flags bit0)。
void OSL_SetNotReady(bool not_ready);

// </editor-fold>

#ifdef	__cplusplus
}
#endif

#endif	/* OSL_SLAVE_H */
