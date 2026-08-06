#include "i2c_master.h"
#include "mcc_generated_files/i2c_host/twi1.h"
#include "mcc_generated_files/i2c_host/i2c_host_event_types.h"  // twi1_Status

#include <avr/io.h>     // TWI1.MSTATUS の参照 (診断用)

// I2C バス完了待ちの上限。TWI0_IsBusy/Tasks を繰り返し回すループの反復回数で
// 表現する。F_CPU = 24 MHz、1 反復あたり 100〜200 cycle 程度として概ね 0.8〜1.6 秒
// 相当。この上限を超えたら通信を諦めてエラー復帰し、呼び出し側にリトライを委ねる。
// WDT (~4s) より十分短く設定していること。
#define I2C_WAIT_MAX_ITER  200000UL

// 診断カウンタ。publish して外から観測する (I2C_GetDiag)
static uint16_t i2c_fail_count    = 0;   // 転送失敗の累計
static uint16_t i2c_recover_count = 0;   // バス強制復帰を実行した累計

static void i2c_count_fail(void)
{
    if (i2c_fail_count < 0xFFFF) i2c_fail_count++;
}

// 内部関数: バスをアイドル状態に強制復帰させる
static void i2c_recover_bus(void)
{
    // TWI を再初期化。ハング状態のバス状態機械を IDLE に戻す。
    TWI1_Deinitialize();
    TWI1_Initialize();

    // TWI1_Initialize()/Deinitialize() はレジスタしか触らず、ドライバ側の状態
    // (twi1_Status) には一切手を付けない。busy を false にするのは TWI1_Close() 等の
    // 静的関数だけで、これらは TWI1_Tasks() が状態機械を最後まで進めたときにしか
    // 呼ばれない。転送が完了しないまま再初期化すると割り込みフラグごと消えるため、
    // 状態機械は二度と前へ進めず busy が真のまま固着する。
    //
    // TWI1_IsBusy() は busy も見るので、これを落とさない限り以降の転送開始は
    // すべて失敗し続け、pod の電源を入れ直すまで全センサの読み出しが止まる。
    // 実機で mst=1 (バスは IDLE) のまま fail と rcv だけが増え続ける状態を観測して
    // 確認した。
    twi1_Status.busy         = false;
    twi1_Status.address      = 0xFF;
    twi1_Status.writePtr     = NULL;
    twi1_Status.readPtr      = NULL;
    twi1_Status.writeLength  = 0;
    twi1_Status.readLength   = 0;
    twi1_Status.switchToRead = false;
    twi1_Status.errorState   = I2C_ERROR_NONE;
    twi1_Status.state        = I2C_STATE_IDLE;

    if (i2c_recover_count < 0xFFFF) i2c_recover_count++;
}

// 内部関数: 転送を開始できなかった場合の後始末。
//
// TWI1_Write/Read/WriteRead が false を返すのは TWI1_IsBusy() が真のときだけで、
// これは twi1_Status.busy が立ちっぱなしか、MSTATUS.BUSSTATE が IDLE 以外に固着
// していることを意味する。BUSSTATE が IDLE に戻るのは「バス上で STOP を観測する」
// か「ソフトウェアが IDLE を書く」かの 2 つだけなので、子機を全て取り外しても
// (STOP を出す相手がいなくなるため) 自然復帰しない。
//
// この経路は wait_for_completion() を通らず、以前はそこにしか復帰処理がなかった。
// そのため一度固着すると全子機の読み出しが即座に失敗し続け、pod の電源を入れ直す
// までデータが止まっていた。ここで明示的に復帰させる。
static bool i2c_start_failed(void)
{
    i2c_count_fail();
    i2c_recover_bus();
    return false;
}

// I2C の診断情報を取得する。不要な項目には NULL を渡してよい。
void I2C_GetDiag(uint8_t *mstatus, uint16_t *failCount, uint16_t *recoverCount)
{
    if (mstatus      != NULL) *mstatus      = TWI1.MSTATUS;
    if (failCount    != NULL) *failCount    = i2c_fail_count;
    if (recoverCount != NULL) *recoverCount = i2c_recover_count;
}

// 内部関数: 転送完了を待ち、エラー状態を確認する
// タイムアウトした場合はバスを強制リセットして false を返す
static bool wait_for_completion(void)
{
    uint32_t iter = 0;

    while (TWI1_IsBusy())
    {
        TWI1_Tasks();
        if (++iter > I2C_WAIT_MAX_ITER)
        {
            // ハング疑い。バスを強制復帰して失敗を返す
            i2c_count_fail();
            i2c_recover_bus();
            return false;
        }
    }

    // エラー状態を確認
    i2c_host_error_t error = TWI1_ErrorGet();

    // エラーがなければ成功 (I2C_ERROR_NONE == 0)
    return (error == I2C_ERROR_NONE);
}

bool I2C_IsConnected(uint8_t address)
{
   // ダミーデータ（送信はされないがポインタとして必要）
    uint8_t dummy = 0;

    // 長さ0で書き込みを試みる (アドレス送信 -> ACKチェック -> Stop)
    // Melodyのバージョンによっては、長さ0だと何もせずtrueを返す場合があるため要確認
    if (!TWI1_Write(address, &dummy, 0)) return i2c_start_failed();

    return wait_for_completion();
}

bool I2C_Write(uint8_t address, const uint8_t *data, size_t len)
{
    // 転送開始要求
    if (!TWI1_Write(address, (uint8_t *)data, len)) return i2c_start_failed(); // 他の転送が進行中

    // 完了待ち
    return wait_for_completion();
}

bool I2C_Read(uint8_t address, uint8_t *buffer, size_t len)
{
    if (len == 0) return true;

    // 転送開始要求
    if (!TWI1_Read(address, buffer, len)) return i2c_start_failed();

    // 完了待ち
    return wait_for_completion();
}

bool I2C_WriteRead(uint8_t address, const uint8_t *writeData, size_t writeLen, uint8_t *readBuffer, size_t readLen)
{
    // MelodyのWriteRead機能を使用
    if (!TWI1_WriteRead(address, (uint8_t *)writeData, writeLen, readBuffer, readLen)) return i2c_start_failed();

    // 完了待ち
    return wait_for_completion();
}

bool I2C_WriteByteAndStop(uint8_t address, uint8_t data)
{
    // Write関数で1バイト送信するのと同義
    return I2C_Write(address, &data, 1);
}
