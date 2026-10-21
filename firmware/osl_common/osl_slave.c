#include "osl_slave.h"

#include "mcc_generated_files/i2c_client/twi0.h"
#include "mcc_generated_files/nvm/nvm.h"
#include "mcc_generated_files/system/ccp.h"

#include <avr/io.h>
#include <util/atomic.h>
#include <string.h>

// <editor-fold defaultstate="collapsed" desc="番地 (仕様 §3, §4)">

#define REG_IDENT           0x0000  // 識別 32 B (CRC 込み)
#define REG_ADDR_KEY        0x0020
#define REG_NEW_ADDR        0x0021
#define REG_NAME            0x0040  // 名前 64 B + CRC 2 B
#define REG_CHDESC          0x0100  // チャネル記述 16 B × N + CRC
#define REG_POLL            0x0300  // POLL (スナップショット)
#define REG_EXT_IDENT       0x0400  // 拡張識別 8 B + CRC
#define REG_EXT_ATTR        0x0420  // 拡張チャネル属性 4 B × N + CRC
#define REG_EXT_STATUS      0x0480  // 拡張ステータス 10 B + CRC (スナップショット)
#define REG_CMD_KEY         0x04C0
#define REG_CMD             0x04C1
#define REG_CMD_ARG         0x04C2
#define REG_USER            0x1000  // ユーザー領域

#define KEY_UNLOCK          0xA5
#define CMD_RESET           0x01
#define CMD_IDENTIFY        0x02

#define I2C_ADDR_MIN        0x08
#define I2C_ADDR_MAX        0x77

#define NAME_LEN            64
#define IDENT_LEN           32
#define EXT_IDENT_LEN       10
#define EXT_STATUS_LEN      12
#define CHDESC_MAX          (16 * OSL_MAX_CHANNELS + 2)
#define EXT_ATTR_MAX        (4 * OSL_MAX_CHANNELS + 2)
#define POLL_MAX            (2 + 5 * OSL_MAX_CHANNELS + 1 + 2)

// node_flags (仕様 §4.4)
#define NF_NOT_READY        0x01
#define NF_CONFIG_LOST      0x02

// reset_cause (仕様 §6.4)
#define RC_UNKNOWN          0
#define RC_POWER_ON         1
#define RC_BROWN_OUT        2
#define RC_WATCHDOG         3
#define RC_SOFTWARE         4
#define RC_EXTERNAL         5
#define RC_OTHER            6

// CLKHOLD がこの時間続いたら固着とみなす [ms]。正常な電文は数 ms で終わる。
#define CLKHOLD_STUCK_MS    1000

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="状態">

static const osl_config_t *s_cfg;

// 読み出し用に組み立てた区画 (CRC 込み)
static uint8_t  s_ident[IDENT_LEN];
static uint8_t  s_name[NAME_LEN + 2];
static uint8_t  s_chdesc[CHDESC_MAX];
static uint16_t s_chdesc_len;
static uint8_t  s_ext_ident[EXT_IDENT_LEN];
static uint8_t  s_ext_attr[EXT_ATTR_MAX];
static uint16_t s_ext_attr_len;

// POLL: 機種側が更新する live と、読み出しに使う shadow。
// 親機が番地 0x0300 を指定した瞬間に live を shadow へ写す。
static uint8_t  s_poll_live[POLL_MAX];
static uint8_t  s_poll_shadow[POLL_MAX];
static uint8_t  s_poll_len;
static uint16_t s_sample_seq;
static uint8_t  s_values[4 * OSL_MAX_CHANNELS];
static uint8_t  s_status[OSL_MAX_CHANNELS];

// 拡張ステータス (番地 0x0480 を指定した瞬間に組み立てる)
static uint8_t           s_ext_status[EXT_STATUS_LEN];
static volatile uint32_t s_uptime_s;
static uint16_t          s_uptime_ms_acc;
static volatile uint8_t  s_node_flags;
static uint8_t           s_reset_cause;
static volatile uint16_t s_err_i2c;
static volatile uint16_t s_err_sensor;
static uint16_t          s_clkhold_ms;

// 現在の I2C アドレス
static volatile uint8_t  s_i2c_addr;

// 取引 (START から STOP まで) の状態。割り込みの中だけで使う。
static uint16_t s_ptr;              // 読み出し / 書き込みの位置
static uint8_t  s_addr_bytes;       // 受け取った番地のバイト数 (0〜2)
static bool     s_was_read;         // この取引で読み出しがあった
static bool     s_wrote_data;       // この取引で番地以外のデータを書いた
static uint8_t  s_addr_key, s_new_addr;
static bool     s_new_addr_written;
static uint8_t  s_cmd_key, s_cmd, s_cmd_arg;
static uint8_t  s_name_stage[NAME_LEN];
static bool     s_name_touched;
static bool     s_user_touched;

// 割り込みからメインループへの依頼
static volatile bool    s_persist_req;
static volatile bool    s_user_changed;
static volatile bool    s_reset_req;
static volatile bool    s_identify_req;
static volatile uint8_t s_identify_sec;

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="小物">

// CRC-16/CCITT-FALSE (多項式 0x1021、初期値 0xFFFF、反転なし)。"123456789" → 0x29B1
static uint16_t crc16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static inline void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

// buf[0..len-1] の CRC を buf[len], buf[len+1] に置く
static void seal(uint8_t *buf, uint16_t len)
{
    put_u16(&buf[len], crc16(buf, len));
}

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="EEPROM">

// EEPROM の形 (EEPROM_START から):
//   +0  'O' 'S'       目印
//   +2  layout (=1)   形の版
//   +3  user_size     ユーザー領域の大きさ (u16)
//   +5  i2c_addr
//   +6  name[64]
//   +70 user[user_size]
//   末尾 CRC-16 (先頭から直前まで)
// CRC か目印か大きさが合わなければ壊れているとみなし、既定値で作り直す。
#define EE_LAYOUT       1
#define EE_HDR_LEN      6
#define EE_NAME_OFS     6
#define EE_USER_OFS     (EE_NAME_OFS + NAME_LEN)

static uint16_t ee_len(void)
{
    return EE_USER_OFS + s_cfg->user_size;
}

static uint8_t ee_read(uint16_t ofs)
{
    while (EEPROM_IsBusy());
    return EEPROM_Read((eeprom_address_t)(EEPROM_START + ofs));
}

// 変わったバイトだけ書く (書き込み回数と時間を減らす)
static void ee_write(uint16_t ofs, uint8_t v)
{
    if (ee_read(ofs) == v) return;
    while (EEPROM_IsBusy());
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        EEPROM_Write((eeprom_address_t)(EEPROM_START + ofs), v);
    }
}

// 現在の設定 (アドレス・名前・ユーザー領域) を EEPROM に保存する。
// 割り込みが書き換える領域なので、先に一括で写し取ってから書く。
static void ee_save(void)
{
    uint8_t addr;
    uint8_t name[NAME_LEN];
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        addr = s_i2c_addr;
        memcpy(name, s_name, NAME_LEN);
    }

    uint16_t crc = 0xFFFF;
    uint16_t len = ee_len();
    for (uint16_t i = 0; i < len; i++) {
        uint8_t v = 0;
        if      (i == 0) v = 'O';
        else if (i == 1) v = 'S';
        else if (i == 2) v = EE_LAYOUT;
        else if (i == 3) v = (uint8_t)s_cfg->user_size;
        else if (i == 4) v = (uint8_t)(s_cfg->user_size >> 8);
        else if (i == 5) v = addr;
        else if (i < EE_USER_OFS) v = name[i - EE_NAME_OFS];
        else {
            ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
                v = ((const uint8_t *)s_cfg->user_region)[i - EE_USER_OFS];
            }
        }
        ee_write(i, v);

        // CRC を 1 バイトずつ進める (crc16() と同じ計算)
        crc ^= (uint16_t)v << 8;
        for (uint8_t b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    ee_write(len,     (uint8_t)crc);
    ee_write(len + 1, (uint8_t)(crc >> 8));
}

// EEPROM を読む。壊れていれば既定値を入れて保存し直し、false を返す。
static bool ee_load(void)
{
    uint16_t len = ee_len();
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        uint8_t v = ee_read(i);
        crc ^= (uint16_t)v << 8;
        for (uint8_t b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    uint16_t stored = (uint16_t)ee_read(len) | ((uint16_t)ee_read(len + 1) << 8);

    bool ok = ee_read(0) == 'O' && ee_read(1) == 'S' && ee_read(2) == EE_LAYOUT
           && ee_read(3) == (uint8_t)s_cfg->user_size
           && ee_read(4) == (uint8_t)(s_cfg->user_size >> 8)
           && crc == stored;

    uint8_t addr = ok ? ee_read(5) : 0;
    if (!ok || addr < I2C_ADDR_MIN || addr > I2C_ADDR_MAX) {
        // 既定値で作り直す
        s_i2c_addr = s_cfg->default_i2c_addr ? s_cfg->default_i2c_addr : OSL_DEFAULT_I2C_ADDR;
        memset(s_name, 0, NAME_LEN);
        if (s_cfg->user_size) {
            memcpy(s_cfg->user_region, s_cfg->user_default, s_cfg->user_size);
        }
        ee_save();
        return false;
    }

    s_i2c_addr = addr;
    for (uint16_t i = 0; i < NAME_LEN; i++) s_name[i] = ee_read(EE_NAME_OFS + i);
    s_name[NAME_LEN - 1] = '\0';
    for (uint16_t i = 0; i < s_cfg->user_size; i++) {
        ((uint8_t *)s_cfg->user_region)[i] = ee_read(EE_USER_OFS + i);
    }
    return true;
}

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="区画の組み立て">

static void build_ident(void)
{
    uint8_t *p = s_ident;
    p[0] = 'O'; p[1] = 'S'; p[2] = 'L';
    p[3] = OSL_SPEC_VERSION;
    put_u16(&p[4], s_cfg->vendor_id);
    put_u16(&p[6], s_cfg->model_id);
    p[8]  = s_cfg->fw_major;
    p[9]  = s_cfg->fw_minor;
    p[10] = s_cfg->fw_patch;
    p[11] = s_cfg->channel_count;
    put_u16(&p[12], s_cfg->update_period_ms);
    // device_id: マイコン固有の 16 バイトのシリアル番号をそのまま使う (仕様 §3.2.1)
    memcpy(&p[14], (const void *)&SIGROW.SERNUM0, 16);
    seal(p, 30);
}

static void build_chdesc(void)
{
    uint8_t n = s_cfg->channel_count;
    memset(s_chdesc, 0, sizeof(s_chdesc));
    for (uint8_t i = 0; i < n; i++) {
        uint8_t *p = &s_chdesc[16 * i];
        const osl_channel_t *c = &s_cfg->channels[i];
        put_u16(&p[0], c->unit);
        p[2] = c->type;
        // 名前は 11 バイトまで。12 バイト目は必ず NUL にする
        if (c->name) strncpy((char *)&p[4], c->name, 11);
    }
    s_chdesc_len = 16 * n + 2;
    seal(s_chdesc, 16 * n);
}

static void build_ext(void)
{
    memset(s_ext_ident, 0, sizeof(s_ext_ident));
    memset(s_ext_attr, 0, sizeof(s_ext_attr));
    s_ext_attr_len = 0;

    if (s_cfg->capability == 0) return;     // 任意区画なし: すべて 0 を返す

    put_u16(&s_ext_ident[0], s_cfg->capability);
    s_ext_ident[2] = s_cfg->hw_rev;
    put_u16(&s_ext_ident[4], s_cfg->avg_window_ms);
    put_u16(&s_ext_ident[6], s_cfg->warmup_ms);
    seal(s_ext_ident, 8);

    if (s_cfg->capability & OSL_CAP_CH_ATTR) {
        uint8_t n = s_cfg->channel_count;
        for (uint8_t i = 0; i < n; i++) {
            s_ext_attr[4 * i]     = s_cfg->channels[i].precision;
            s_ext_attr[4 * i + 1] = s_cfg->channels[i].flags;
        }
        s_ext_attr_len = 4 * n + 2;
        seal(s_ext_attr, 4 * n);
    }
}

// s_values / s_status から POLL を組み立てて live に写す。
// CRC の計算は割り込みを止めずに行い、写す瞬間だけ止める。
static void build_poll(void)
{
    uint8_t buf[POLL_MAX];
    uint8_t n = s_cfg->channel_count;
    uint8_t len = 0;

    put_u16(&buf[0], s_sample_seq);
    memcpy(&buf[2], s_values, 4 * n);
    memcpy(&buf[2 + 4 * n], s_status, n);
    len = 2 + 5 * n;
    if (len & 1) buf[len++] = 0;            // CRC を偶数境界にそろえる
    seal(buf, len);

    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        memcpy(s_poll_live, buf, len + 2);
    }
}

// 拡張ステータスのスナップショット (割り込みの中で呼ぶ)
static void latch_ext_status(void)
{
    uint8_t *p = s_ext_status;
    uint32_t up = s_uptime_s;
    p[0] = (uint8_t)up;
    p[1] = (uint8_t)(up >> 8);
    p[2] = (uint8_t)(up >> 16);
    p[3] = (uint8_t)(up >> 24);
    p[4] = s_node_flags;
    p[5] = s_reset_cause;
    put_u16(&p[6], s_err_i2c);
    put_u16(&p[8], s_err_sensor);
    seal(p, 10);
}

// 起動要因を読み、フラグを消す (次の起動で正しく判定するため)
static uint8_t read_reset_cause(void)
{
    uint8_t f = RSTCTRL.RSTFR;
    RSTCTRL.RSTFR = f;
    if (f & RSTCTRL_PORF_bm)  return RC_POWER_ON;
    if (f & RSTCTRL_BORF_bm)  return RC_BROWN_OUT;
    if (f & RSTCTRL_WDRF_bm)  return RC_WATCHDOG;
    if (f & RSTCTRL_SWRF_bm)  return RC_SOFTWARE;
    if (f & RSTCTRL_EXTRF_bm) return RC_EXTERNAL;
    if (f)                    return RC_OTHER;      // UPDI など
    return RC_UNKNOWN;
}

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="I2C クライアント (割り込みの中)">

static uint8_t read_byte(uint16_t a)
{
    if (a < REG_IDENT + IDENT_LEN) return s_ident[a];
    if (a >= REG_NAME && a < REG_NAME + NAME_LEN + 2) return s_name[a - REG_NAME];
    if (a >= REG_CHDESC && a < REG_CHDESC + s_chdesc_len) return s_chdesc[a - REG_CHDESC];
    if (a >= REG_POLL && a < REG_POLL + s_poll_len) return s_poll_shadow[a - REG_POLL];
    if (a >= REG_EXT_IDENT && a < REG_EXT_IDENT + EXT_IDENT_LEN) return s_ext_ident[a - REG_EXT_IDENT];
    if (a >= REG_EXT_ATTR && a < REG_EXT_ATTR + s_ext_attr_len) return s_ext_attr[a - REG_EXT_ATTR];
    if ((s_cfg->capability & OSL_CAP_EXT_STATUS)
        && a >= REG_EXT_STATUS && a < REG_EXT_STATUS + EXT_STATUS_LEN) {
        return s_ext_status[a - REG_EXT_STATUS];
    }
    if (a >= REG_USER && (uint16_t)(a - REG_USER) < s_cfg->user_size) {
        return ((const uint8_t *)s_cfg->user_region)[a - REG_USER];
    }
    return 0;       // 未実装・予約は 0 (仕様 §1)
}

static void write_byte(uint16_t a, uint8_t v)
{
    if (a == REG_ADDR_KEY) {
        s_addr_key = v;
    } else if (a == REG_NEW_ADDR) {
        s_new_addr = v;
        s_new_addr_written = true;
    } else if (a >= REG_NAME && a < REG_NAME + NAME_LEN) {
        // 名前は STOP で一括して反映する (書き込み途中の名前を読ませない)
        if (!s_name_touched) {
            memcpy(s_name_stage, s_name, NAME_LEN);
            s_name_touched = true;
        }
        s_name_stage[a - REG_NAME] = v;
    } else if (a == REG_CMD_KEY) {
        s_cmd_key = v;
    } else if (a == REG_CMD) {
        s_cmd = v;
    } else if (a == REG_CMD_ARG) {
        s_cmd_arg = v;
    } else if (a >= REG_USER && s_cfg->user_writable
               && (uint16_t)(a - REG_USER) < s_cfg->user_size) {
        ((uint8_t *)s_cfg->user_region)[a - REG_USER] = v;
        s_user_touched = true;
    }
    // 読み取り専用・未実装の番地への書き込みは無視する (ACK は返す)
}

// 番地が確定した瞬間に、スナップショットを取る区画を写す
static void on_address_set(uint16_t a)
{
    if (a == REG_POLL) {
        memcpy(s_poll_shadow, s_poll_live, s_poll_len);
    } else if (a == REG_EXT_STATUS) {
        latch_ext_status();
    }
}

// 取引の終わり (STOP、または異常での打ち切り)
static void end_transaction(void)
{
    // アドレス変更: 鍵と範囲を確かめ、すぐに切り替える。EEPROM への保存はメインで。
    if (s_addr_key == KEY_UNLOCK && s_new_addr_written
        && s_new_addr >= I2C_ADDR_MIN && s_new_addr <= I2C_ADDR_MAX) {
        s_i2c_addr  = s_new_addr;
        TWI0.SADDR  = (uint8_t)(s_new_addr << 1);
        s_persist_req = true;
    }
    s_addr_key = 0;
    s_new_addr_written = false;

    // 名前: 反映して CRC を作り直す
    if (s_name_touched) {
        memcpy(s_name, s_name_stage, NAME_LEN);
        s_name[NAME_LEN - 1] = '\0';
        seal(s_name, NAME_LEN);
        s_name_touched = false;
        s_persist_req  = true;
    }

    if (s_user_touched) {
        s_user_touched = false;
        s_user_changed = true;
        s_persist_req  = true;
    }

    // コマンド
    if (s_cmd_key == KEY_UNLOCK) {
        if (s_cmd == CMD_RESET && (s_cfg->capability & OSL_CAP_CMD_RESET)) {
            s_reset_req = true;
        } else if (s_cmd == CMD_IDENTIFY && (s_cfg->capability & OSL_CAP_CMD_IDENTIFY)) {
            s_identify_sec = s_cmd_arg;
            s_identify_req = true;
        }
    }
    s_cmd_key = 0;
    s_cmd = 0;

    // 読み出し位置の規則 (仕様 §1.2): 番地だけを書いた取引の後は保持し、
    // 読み出しやデータ付きの書き込みの後は 0x0000 に戻す。
    // 番地の 1 バイト目だけで終わった取引も 0x0000 に戻す。
    if (s_was_read || s_wrote_data || s_addr_bytes != 2) s_ptr = 0;
    s_was_read   = false;
    s_wrote_data = false;
    s_addr_bytes = 0;
}

static bool on_i2c_event(i2c_client_transfer_event_t ev)
{
    switch (ev) {
        case I2C_CLIENT_TRANSFER_EVENT_ADDR_MATCH:
            if (TWI0_TransferDirGet() == I2C_CLIENT_TRANSFER_DIR_WRITE) {
                // 書き込みの開始: 最初の 2 バイトが番地
                s_addr_bytes = 0;
            } else {
                // 読み出しの開始 (Repeated START でも、番地なしでも同じ)
                s_was_read = true;
            }
            break;

        case I2C_CLIENT_TRANSFER_EVENT_RX_READY: {
            uint8_t b = TWI0_ReadByte();
            if (s_addr_bytes == 0) {
                s_ptr = (uint16_t)b << 8;           // 番地の上位
                s_addr_bytes = 1;
            } else if (s_addr_bytes == 1) {
                s_ptr |= b;                         // 番地の下位
                s_addr_bytes = 2;
                on_address_set(s_ptr);
            } else {
                write_byte(s_ptr++, b);
                s_wrote_data = true;
            }
            break;
        }

        case I2C_CLIENT_TRANSFER_EVENT_TX_READY:
            TWI0_WriteByte(read_byte(s_ptr++));
            break;

        case I2C_CLIENT_TRANSFER_EVENT_STOP_BIT_RECEIVED:
            end_transaction();
            break;

        case I2C_CLIENT_TRANSFER_EVENT_ERROR:
            // 生成ドライバは COLL / BUSERR を検出してもフラグを消さず SCMD も書かない。
            // クライアントが SCL を離す手段は SCMD しかないので、ここで自分で解放する。
            // (v1 の 2026-08-10 の実測: これを怠るとバス上の全ノードが止まる)
            s_err_i2c++;
            TWI0.SSTATUS = (TWI_COLL_bm | TWI_BUSERR_bm);
            TWI0.SCTRLB  = TWI_ACKACT_NACK_gc | TWI_SCMD_COMPTRANS_gc;
            end_transaction();
            break;

        default:
            break;
    }
    return true;    // 常に ACK
}

// </editor-fold>

// <editor-fold defaultstate="collapsed" desc="公開関数">

void OSL_Init(const osl_config_t *cfg)
{
    s_cfg = cfg;
    s_reset_cause = read_reset_cause();

    if (!ee_load()) s_node_flags |= NF_CONFIG_LOST;
    seal(s_name, NAME_LEN);

    build_ident();
    build_chdesc();
    build_ext();

    // 起動直後は値が無いので、全チャネルを準備中にしておく
    uint8_t n = cfg->channel_count;
    memset(s_values, 0, sizeof(s_values));
    memset(s_status, OSL_ST_NOT_READY, sizeof(s_status));
    s_poll_len = (uint8_t)(2 + 5 * n + ((5 * n) & 1) + 2);
    build_poll();
    memcpy(s_poll_shadow, s_poll_live, s_poll_len);

    TWI0.SADDR = (uint8_t)(s_i2c_addr << 1);
    TWI0_CallbackRegister(on_i2c_event);
}

void OSL_Task(void)
{
    if (s_user_changed) {
        s_user_changed = false;
        if (s_cfg->on_user_changed) s_cfg->on_user_changed();
    }
    if (s_persist_req) {
        s_persist_req = false;
        ee_save();
    }
    if (s_identify_req) {
        s_identify_req = false;
        if (s_cfg->on_identify) s_cfg->on_identify(s_identify_sec);
    }
    if (s_reset_req) {
        // STOP を受け取ってから実行するので、親機との取引は終わっている
        ccp_write_io((void *)&RSTCTRL.SWRR, RSTCTRL_SWRST_bm);
    }
}

void OSL_Tick(uint16_t elapsed_ms)
{
    s_uptime_ms_acc += elapsed_ms;
    while (s_uptime_ms_acc >= 1000) {
        s_uptime_ms_acc -= 1000;
        s_uptime_s++;
    }

    // CLKHOLD 固着の監視。生成ドライバには SCMD を書かずに ISR を抜ける経路が残って
    // いて、そこに入ると SCL を離さないまま止まり、バス上の全ノードが読めなくなる。
    // 正常な電文は数 ms で終わるので、1 秒続いたら異常と断定して取引を打ち切る。
    if (TWI0.SSTATUS & TWI_CLKHOLD_bm) {
        s_clkhold_ms += elapsed_ms;
        if (s_clkhold_ms >= CLKHOLD_STUCK_MS) {
            ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
                s_err_i2c++;
                TWI0.SSTATUS = (TWI_COLL_bm | TWI_BUSERR_bm | TWI_APIF_bm | TWI_DIF_bm);
                TWI0.SCTRLB  = TWI_ACKACT_NACK_gc | TWI_SCMD_COMPTRANS_gc;
                end_transaction();
            }
            s_clkhold_ms = 0;
        }
    } else {
        s_clkhold_ms = 0;
    }
}

void OSL_Publish(const void *values, const uint8_t *status)
{
    uint8_t n = s_cfg->channel_count;
    if (values) {
        memcpy(s_values, values, 4 * n);
        s_sample_seq++;
    }
    if (status) memcpy(s_status, status, n);
    build_poll();
}

void OSL_FactoryReset(void)
{
    uint8_t addr = s_cfg->default_i2c_addr ? s_cfg->default_i2c_addr : OSL_DEFAULT_I2C_ADDR;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        s_i2c_addr = addr;
        TWI0.SADDR = (uint8_t)(addr << 1);
        memset(s_name, 0, NAME_LEN);
        seal(s_name, NAME_LEN);
        if (s_cfg->user_size) {
            memcpy(s_cfg->user_region, s_cfg->user_default, s_cfg->user_size);
        }
    }
    ee_save();
    if (s_cfg->on_user_changed) s_cfg->on_user_changed();
}

void OSL_CountSensorError(void)
{
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        s_err_sensor++;
    }
}

void OSL_SetNotReady(bool not_ready)
{
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        if (not_ready) s_node_flags |= NF_NOT_READY;
        else           s_node_flags &= (uint8_t)~NF_NOT_READY;
    }
}

// </editor-fold>
