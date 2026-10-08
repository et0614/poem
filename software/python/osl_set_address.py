"""
OSL 子機 (温湿度・風速・絶対圧・差圧センサ) の I2C アドレスを変更する。

usage:
    python osl_set_address.py 0x10 0x11          0x10 の子機を 0x11 に変える (確認あり)
    python osl_set_address.py 0x10 0x11 --yes    確認せずに変える
    python osl_set_address.py 0x10 0x11 --bridge cp2112

手順 (共通レジスタ仕様、firmware/*/i2c_slave.c):
  REG_ADDR_KEY (0x04) に鍵 0xA5、続けて REG_NEW_ADDR (0x05) に新アドレスを
  「1 回の書き込み」で送る。子機は STOP を受けた時点で EEPROM に保存し、
  TWI のアドレスも切り替える。リセットは要らず、次の通信から新アドレスで応答する。
  鍵は STOP のたびに消えるので、鍵と新アドレスを別々の書き込みにすると
  新アドレスは黙って無視される。

注意:
  - PoEMServer の BACnet オブジェクト番号は I2C アドレス × 100 なので、アドレスを
    変えるとその子機の番号も変わる。
  - 親機につないだまま変えた場合、親機は新アドレスを 10 秒以内に新しい子機として
    見つけるが、旧アドレスは「応答なし」のまま一覧に残る。親機の HTTP 設定画面で
    SCAN をやり直すと消える。
"""
import argparse
import sys
import time

from osl_reader import REG_INFO, open_bridge, read_info, sensor_type, read_poll

REG_ADDR_KEY    = 0x04
ADDR_KEY_UNLOCK = 0xA5
ADDR_MIN        = 0x08
ADDR_MAX        = 0x77


def responds(dev, addr: int) -> bool:
    dev.slave_addr = addr
    return dev.read_i2c_block(REG_INFO, 1, retries=1) is not None


def main():
    ap = argparse.ArgumentParser(description='OSL 子機の I2C アドレスを変更する')
    ap.add_argument('old', type=lambda s: int(s, 0), help='現在のアドレス (例: 0x10)')
    ap.add_argument('new', type=lambda s: int(s, 0), help='新しいアドレス (0x08-0x77)')
    ap.add_argument('--yes', action='store_true', help='確認せずに実行する')
    ap.add_argument('--bridge', choices=('auto', 'mcp2221', 'cp2112'), default='auto')
    ap.add_argument('--serial', help='ブリッジのシリアル番号 (複数接続時)')
    args = ap.parse_args()

    old, new = args.old, args.new
    if not ADDR_MIN <= new <= ADDR_MAX:
        print(f'新しいアドレスは 0x{ADDR_MIN:02X}-0x{ADDR_MAX:02X} の範囲で指定してください。')
        return 1
    if old == new:
        print('現在のアドレスと同じです。')
        return 1

    dev = open_bridge(args.bridge, args.serial)
    if dev is None:
        return 1

    try:
        # 対象の子機を確認
        dev.slave_addr = old
        info = read_info(dev)
        if info is None:
            print(f'0x{old:02X} に子機が見つかりません。')
            return 1
        poll = read_poll(dev)
        name, _ = sensor_type(info, poll)
        print(f'対象: 0x{old:02X} {name}  Device ID {info["id"]}  Name "{info["name"]}"')

        # 新アドレスに別の機器がいると、同じアドレスが 2 台になって両方読めなくなる
        if responds(dev, new):
            print(f'0x{new:02X} にはすでに別の機器が応答しています。別のアドレスを選んでください。')
            return 1

        if not args.yes:
            ans = input(f'0x{old:02X} → 0x{new:02X} に変更します。よろしいですか? [y/N] ')
            if ans.strip().lower() not in ('y', 'yes'):
                print('中止しました。')
                return 1

        # 鍵と新アドレスを 1 回の書き込みで送る
        dev.slave_addr = old
        if not dev.write_i2c_block(REG_ADDR_KEY, [ADDR_KEY_UNLOCK, new]):
            print('書き込みに失敗しました。アドレスは変わっていません。')
            return 1

        # 子機は STOP で EEPROM に書いてから切り替える。少し待ってから確かめる
        time.sleep(0.1)
        dev.slave_addr = new
        moved = read_info(dev)
        if moved is None or moved['id'] != info['id'] or moved['addr'] != new:
            print(f'0x{new:02X} で子機を確認できませんでした。'
                  f'python osl_reader.py でバスを走査して状態を確かめてください。')
            return 1
        if responds(dev, old):
            print(f'注意: 0x{old:02X} にまだ応答する機器があります。')

        print(f'変更しました: 0x{old:02X} → 0x{new:02X} (Device ID {moved["id"]})')
        print('EEPROM に保存済みなので、電源を入れ直しても新しいアドレスのままです。')
        print(f'PoEMServer の BACnet オブジェクト番号は {old * 100}- から {new * 100}- に変わります。')
        return 0
    finally:
        dev.close()


if __name__ == '__main__':
    sys.exit(main())
