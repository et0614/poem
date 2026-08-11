#include "i2c_master.h"
#include "mcc_generated_files/i2c_host/twi1.h"
#include "mcc_generated_files/i2c_host/i2c_host_event_types.h"  // twi1_Status

#include <avr/io.h>     // TWI1.MSTATUS の参照 (診断用)
#include "mcc_generated_files/system/clock.h"   // F_CPU (_delay_us が要求する)
#include <util/delay.h>

// I2C バス完了待ちの上限。TWI1_IsBusy/Tasks を繰り返し回すループの反復回数で
// 表現する。F_CPU = 24 MHz、1 反復あたり 100〜200 cycle 程度として、この値で
// 概ね 40〜80 ミリ秒相当。
//
// 正常なポーリングは 1 バイト書いて 36 バイト読むだけで、100kHz なら 3〜4 ミリ秒
// で完了する。10 倍以上の余裕があるので、正常な転送を誤って打ち切ることはない。
//
// 以前は 200000 (約 1 秒) だったが、長く待っても得るものがない。センサは自分の
// ペースで計測を続けて最新値を保持しているので、1 回読み損ねても次の周期で最新の
// 値が取れる。むしろ待っているあいだ他のセンサの順番が遅れ、1 回のバス異常で
// 巻き添えになる台数が増えていた。実測では 1 台あたり 1 秒かかり、1 回の異常で
// 5 秒ぶんのデータ更新が遅れていた。
#define I2C_WAIT_MAX_ITER  10000UL

// 診断カウンタ。publish して外から観測する (I2C_GetDiag)
//
// 失敗は原因別に数える。合計 1 本しか無かったときは、増分を見ても
//   ・バスが数秒止まる外乱 (タイムアウトの連続。約 10 回/秒 でしか増えない)
//   ・転送を開始できずに即失敗し続ける状態 (待たないので一気に増える)
// の区別がつかなかった。実測で 1 秒に 100 以上増えた事例があり、そのときも
// どちらなのか判断できず、解釈を三度変える羽目になった。
static uint16_t i2c_fail_count    = 0;   // 失敗の合計 = timeout + start
static uint16_t i2c_fail_timeout  = 0;   // 内訳: 完了待ちタイムアウト
static uint16_t i2c_fail_start    = 0;   // 内訳: 転送を開始できなかった
static uint16_t i2c_recover_count = 0;   // バス強制復帰を実行した累計

// 直近の失敗時の MSTATUS。復帰処理がレジスタを初期化する前に控える。
static uint8_t  i2c_fail_mstatus  = 0;

// 復帰処理に入った時点で SDA / SCL が Low に張り付いていた回数。
//
// これが増えるなら「子機がSDA/SCL を Low に駆動したまま解放していない」が実測で確定する。MSTATUS には
// この情報が出ない (ホスト側の BUSERR/ARBLOST/CLKHOLD はどれも立たない) ので、
// ピンを直接読む以外に確かめる方法がない。
//
// 2 本を分けて数えるのは、解放されていない線で原因が変わるため。
//   SDA が Low  → 子機がバイト送信の途中で止まっている (データを吐かせれば解ける)
//   SCL が Low  → 子機がクロックを伸ばしたまま止まっている (外から解く手段がない)
static uint16_t i2c_sda_low_count = 0;
static uint16_t i2c_scl_low_count = 0;

// TWI1 の割り当てピン (AVR128DB32 既定): PF2 = SDA, PF3 = SCL
#define I2C_SDA_bm  PIN2_bm
#define I2C_SCL_bm  PIN3_bm

// 内部関数: バスを物理的に解放させる (I2C の標準手順)
//
// 生成ドライバの Deinitialize/Initialize はホスト側のレジスタしか触らないため、
// 子機が SDA を解放せずに停止している場合は何度やっても解けない。実測でも 43 回
// 繰り返して 4 秒間直らなかった。
//
// 子機は「クロックを送ってもらう」ことと「STOP を受け取ること」でしか状態機械を
// 進められない。そこで TWI を切り離し、SCL を GPIO として叩いて残りのビットを
// 吐き出させ、最後に手で STOP を作る。
//
// クライアント側の生成ドライバ (twi0.c) は COLL/BUSERR を自力でクリアせず、
// これらが落ちるのは STOP 受信時の SSTATUS 書き戻しだけである。つまり STOP を
// 与えることが、子機をエラー状態から出す唯一の手段になっている。
static void i2c_bus_clear(void)
{
    // オープンドレインを手で再現する。OUT は 0 に固定し DIR だけを操作して、
    // Low = 出力 / High = 入力 (プルアップに任せる) とする。High を driving すると
    // 子機がクロックを伸ばしている最中に電流を流し合うことになる。
    PORTF.OUTCLR = I2C_SDA_bm | I2C_SCL_bm;
    PORTF.DIRCLR = I2C_SDA_bm | I2C_SCL_bm;
    _delay_us(5);

    uint8_t pins = PORTF.IN;
    if (!(pins & I2C_SDA_bm)) { if (i2c_sda_low_count < 0xFFFF) i2c_sda_low_count++; }
    if (!(pins & I2C_SCL_bm)) { if (i2c_scl_low_count < 0xFFFF) i2c_scl_low_count++; }

    // 最大 9 発。1 バイト + ACK ぶんに相当する。回数固定なのでここでは止まらない。
    for (uint8_t i = 0; i < 9; i++)
    {
        if (PORTF.IN & I2C_SDA_bm) break;   // 解放された
        PORTF.DIRSET = I2C_SCL_bm;          // SCL Low
        _delay_us(5);
        PORTF.DIRCLR = I2C_SCL_bm;          // SCL High (解放)
        _delay_us(5);
    }

    // STOP を作る。SDA を下げるのは SCL が Low のあいだに行う。SCL が High の
    // ときに下げると START になってしまう。
    PORTF.DIRSET = I2C_SCL_bm;              // SCL Low
    _delay_us(5);
    PORTF.DIRSET = I2C_SDA_bm;              // SDA Low
    _delay_us(5);
    PORTF.DIRCLR = I2C_SCL_bm;              // SCL High
    _delay_us(5);
    PORTF.DIRCLR = I2C_SDA_bm;              // SDA High = STOP
    _delay_us(5);
}

// 内訳と合計を同時に進める。合計を残してあるのは、過去に取得した長期データと
// 比較できるようにするため。
static void i2c_count_fail(uint16_t *breakdown)
{
    if (*breakdown     < 0xFFFF) (*breakdown)++;
    if (i2c_fail_count < 0xFFFF) i2c_fail_count++;
}

// 内部関数: バスをアイドル状態に強制復帰させる
static void i2c_recover_bus(void)
{
    // 復帰する前に MSTATUS を控える。この 1 行より後では「何が起きていたか」を
    // 示す情報がどこにも残らない。TWI1_Deinitialize()/Initialize() は BUSSTATE を
    // IDLE に戻し、BUSERR / ARBLOST / CLKHOLD / RXACK をすべて落としてしまうため、
    // publish 時に読める MSTATUS は「復帰直後の綺麗な値」でしかなく、診断に使えない。
    // 実機で何度観測しても mst=1 か 17 しか出てこなかったのはこのせいだった。
    i2c_fail_mstatus = TWI1.MSTATUS;

    // TWI を再初期化。ハング状態のバス状態機械を IDLE に戻す。
    // 切り離してからバスを物理的に解放させ、そのうえで初期化する。
    TWI1_Deinitialize();
    i2c_bus_clear();
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
    i2c_count_fail(&i2c_fail_start);
    i2c_recover_bus();
    return false;
}

// I2C の診断情報を取得する。
void I2C_GetDiag(i2c_diag_t *diag)
{
    if (diag == NULL) return;

    diag->mstatus      = TWI1.MSTATUS;
    diag->failMstatus  = i2c_fail_mstatus;
    diag->failCount    = i2c_fail_count;
    diag->failTimeout  = i2c_fail_timeout;
    diag->failStart    = i2c_fail_start;
    diag->recoverCount = i2c_recover_count;
    diag->sdaLowCount  = i2c_sda_low_count;
    diag->sclLowCount  = i2c_scl_low_count;
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
            i2c_count_fail(&i2c_fail_timeout);
            i2c_recover_bus();
            return false;
        }
    }

    // 転送が終わっても、STOP はまだバス上を流れている可能性がある。
    //
    // 生成ドライバの I2C_EVENT_STOP() は STOP を「命令」した直後に TWI1_Close() を
    // 呼び、そこで busy = false と MSTATUS = BUSSTATE_IDLE を書いてしまう。つまり
    // STOP が物理的に完了する前に TWI1_IsBusy() が false を返す。pod は 9 台を
    // 連続でポーリングしていて転送の間隔がほぼ無いので、STOP が終わらないうちに
    // 次の START を撃つ余地がある。
    //
    // 実測では、失敗した瞬間の MSTATUS が BUSSTATE=OWNER のまま WIF も RIF も
    // 立っておらず (fm=2)、そこから抜ける手段がドライバ側に存在しない
    // (割り込み無効・ドライバ内タイムアウト無し・ハードの TIMEOUT も DISABLED)。
    // START がバスの整わないうちに出て応答が返らない、という筋と符合する。
    //
    // 待ち時間は 1 ミリ秒とする。100kHz なら 1 ビット 10us、規格上の bus free time
    // (tBUF) は 4.7us なので、STOP の完了とバス開放には 50us もあれば足りる。それでも
    // 桁を上げてあるのは、判定を明確にするため。50us で試したところ変化が見られず、
    // このとき「待ちが足りなかっただけ」という逃げ道が残ってしまった。ここまで待って
    // 頻度が変わらなければ、STOP の完了待ちが原因という筋は否定できる。
    //
    // ポーリングは 1 秒周期のタイマ駆動で、その中で転送を連続で撃って残りは遊んで
    // いる。待ちを入れても転送の回数は変わらず間隔が空くだけなので、「遅くしたから
    // 失敗の機会が減った」という交絡は入らない。
    //
    // 増える時間は、センサ 1 台につきポーリング (I2C_WriteRead) 1 回と、新値を観測した
    // ときの status2 クリア書き込み 1 回で、9 台なら最大 18 転送 = 18 ミリ秒/秒。
    // これに加えて 10 秒ごとの自動スキャンが 0x08〜0x77 の 112 個から既知を除いた
    // 約 103 アドレスを叩くので、その 1 秒だけ 103 ミリ秒増える。ならして約 28
    // ミリ秒/秒 (2.8%)。スキャンの秒も 1 秒の周期に収まる。
    _delay_ms(1);

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
