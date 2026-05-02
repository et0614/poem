"""
CP2112 経由で I2C バス上の応答するスレーブアドレスを列挙する。
各アドレスへ 1 バイト読み出しを試行し、CP2112 の Transfer Status で
ACK/NACK を判別する。

usage:
    python i2c_scan.py [serial]
"""
import sys
import time
import hid
from cp2112_driver import CP2112Device, CP2112Report, I2CStatus


def scan(dev: hid.device, slave_addr: int) -> str:
    """
    1 アドレスへの read を試行して結果を返す。
      'ACK'   : 応答あり
      'NACK'  : 応答なし
      'BUSY'  : ステータス取得失敗
      'ERR'   : 例外
    """
    try:
        # 1 byte read 要求
        dev.write([
            CP2112Report.DATA_WRITE_READ_REQ,
            slave_addr << 1,
            0x00, 0x01,  # length = 1
            0x01, 0x00   # target addr len + offset
        ])

        # 状態ポーリング (BUSY のときは CP2112 が NACK を検出するまで最大 ~250ms 待つ)
        last_status = None
        last_detail = None
        for _ in range(250):
            dev.write([CP2112Report.TRANSFER_STATUS_REQ, 0x01])
            res = dev.read(64, 100)
            if res and res[0] == CP2112Report.TRANSFER_STATUS_RESPONSE:
                last_status = res[1]
                last_detail = res[2] if len(res) > 2 else None
                if last_status in (I2CStatus.COMPLETE, I2CStatus.ERROR, I2CStatus.IDLE):
                    break
            time.sleep(0.001)

        # COMPLETE = 通信完了 (ACK 受信して length 分受け取った)
        # ERROR   = NACK or バス異常
        # 念のため transfer をキャンセル
        dev.write([CP2112Report.CANCEL_TRANSFER, 0x01])
        time.sleep(0.001)

        if last_status == I2CStatus.COMPLETE:
            return f'ACK  (status=0x{last_status:02X}, detail=0x{last_detail:02X})'
        elif last_status == I2CStatus.ERROR:
            # detail bit:
            #  0x01 : Timeout addr ACK
            #  0x02 : Timeout bus
            #  0x03 : Arbitration lost
            #  0x04 : Read incomplete
            #  0x05 : Write incomplete
            #  0x06 : Succeeded after retries
            return f'NACK (status=0x{last_status:02X}, detail=0x{last_detail:02X})'
        elif last_status is None:
            return 'no status response'
        else:
            return f'BUSY (status=0x{last_status:02X})'

    except Exception as e:
        return f'EXC: {e}'


def main():
    serial = sys.argv[1] if len(sys.argv) > 1 and sys.argv[1] != '-' else None

    devices = CP2112Device.list_devices()
    if not devices:
        print('No CP2112 devices found.')
        return 1

    target = None
    if serial:
        for d in devices:
            if d['serial'] == serial:
                target = d
                break
        if target is None:
            print(f'CP2112 with serial "{serial}" not found.')
            return 1
    else:
        target = devices[0]

    print(f'CP2112: {target["product"]} (S/N: {target["serial"]})')

    raw = hid.device()
    raw.open_path(target['path'].encode() if isinstance(target['path'], str) else target['path'])

    # CP2112 を一度リセット
    raw.write([CP2112Report.CANCEL_TRANSFER, 0x01])
    time.sleep(0.01)

    print('\nProbing 7-bit addresses 0x08..0x77 ...')
    found = []
    for addr in range(0x08, 0x78):
        result = scan(raw, addr)
        marker = '<<<' if result.startswith('ACK') else ''
        print(f'  0x{addr:02X}: {result} {marker}')
        if result.startswith('ACK'):
            found.append(addr)

    raw.close()

    print()
    if found:
        print('Devices responding:')
        for a in found:
            print(f'  0x{a:02X}')
    else:
        print('No I2C devices responded. Check wiring / pull-ups / target power.')

    return 0


if __name__ == '__main__':
    sys.exit(main())
