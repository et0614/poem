"""
OSL v2 子機の設定を変える (I2C アドレス・名前・ユーザー領域の値・リセット)。

usage:
    python osl_set.py 0x10 --name "3F 会議室 天井"     名前を変える (UTF-8、63 バイトまで)
    python osl_set.py 0x10 --set dp_b=-0.12             ユーザー領域の値を変える (複数指定可)
    python osl_set.py 0x10 --addr 0x11                  I2C アドレスを 0x11 に変える (確認あり)
    python osl_set.py 0x10 --addr 0x11 --yes            確認せずに変える
    python osl_set.py 0x10 --reset                      ソフトウェアリセット
    python osl_set.py 0x10 --identify 10                識別表示 (LED を 10 秒点滅。LED のある機種のみ)
    python osl_set.py 0x10 --bridge cp2112

ユーザー領域の項目名は機種ごとに違う (osl.py の MODELS)。osl_reader.py で現在値を確認できる。

注意:
  - 親機につないだままアドレスを変えると、親機は新アドレスを次の増分走査で新しい子機として
    見つけるが、旧アドレスは「応答なし」のまま残る。親機で全走査をやり直すと消える。
  - 新品の子機は既定のアドレス (0x10) で出荷されるので、1 台ずつつないで変えること。
"""
import argparse
import sys
import time

import osl

# 子機の名前などに cp932 で表せない文字があっても、コンソール表示で落ちないようにする
if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(errors='replace')


def parse_set(items: list) -> list:
    pairs = []
    for item in items or []:
        if '=' not in item:
            raise SystemExit(f'--set は 名前=値 の形で指定してください: {item}')
        k, v = item.split('=', 1)
        pairs.append((k.strip(), v.strip()))
    return pairs


def main():
    ap = argparse.ArgumentParser(description='OSL v2 子機の設定を変える')
    ap.add_argument('target', type=lambda s: int(s, 0), help='子機の I2C アドレス')
    ap.add_argument('--name', help='名前 (UTF-8、63 バイトまで)')
    ap.add_argument('--set', action='append', metavar='NAME=VALUE', help='ユーザー領域の値')
    ap.add_argument('--addr', type=lambda s: int(s, 0), help='新しい I2C アドレス (0x08〜0x77)')
    ap.add_argument('--reset', action='store_true', help='ソフトウェアリセット')
    ap.add_argument('--identify', type=int, metavar='SEC', help='識別表示の秒数 (0 で停止)')
    ap.add_argument('--yes', action='store_true', help='アドレス変更の確認を省く')
    ap.add_argument('--bridge', choices=('auto', 'mcp2221', 'cp2112'), default='auto')
    ap.add_argument('--serial', help='ブリッジのシリアル番号 (複数接続時)')
    args = ap.parse_args()

    bus = osl.open_bridge(args.bridge, args.serial)
    if bus is None:
        print('USB-I2C ブリッジが見つかりません。')
        return 1
    try:
        dev = osl.OslDevice(bus, args.target)
        try:
            ident = dev.identity()
        except osl.OslError as e:
            print(e)
            return 1
        print(f'[0x{dev.addr:02X}] {ident["model_name"]}  "{dev.name()}"')

        if args.name is not None:
            ok = dev.set_name(args.name)
            time.sleep(0.8)     # 子機は EEPROM に 1 バイト約 10 ms で書く
            print(f'名前: "{dev.name()}"' if ok else '名前の書き込みに失敗しました')

        for k, v in parse_set(args.set):
            fields = {n: fmt for n, _o, fmt, _u in dev.user_fields() or []}
            if k not in fields:
                print(f'{k}: この機種のユーザー領域に無い項目です ({", ".join(fields) or "項目なし"})')
                return 1
            value = float(v) if fields[k] == 'f' else int(v, 0)
            ok = dev.write_user(k, value)
            time.sleep(0.1)
            print(f'{k} = {dev.read_user()[k]:g}' if ok else f'{k} の書き込みに失敗しました')

        if args.identify is not None:
            ext = dev.ext_identity()
            if not ext or not ext['capability'] & osl.CAP_CMD_IDENTIFY:
                print('この機種は識別表示に対応していません')
            else:
                print('識別表示: ' + ('OK' if dev.identify(args.identify) else '失敗'))

        if args.addr is not None:
            if not 0x08 <= args.addr <= 0x77:
                print('アドレスは 0x08〜0x77 で指定してください')
                return 1
            if bus.probe(args.addr):
                print(f'0x{args.addr:02X} には既に別の機器がいます。中止します。')
                return 1
            if not args.yes:
                if input(f'0x{dev.addr:02X} → 0x{args.addr:02X} に変えます。よろしいですか [y/N] ').lower() != 'y':
                    return 0
            if not dev.set_address(args.addr):
                print('書き込みに失敗しました')
                return 1
            time.sleep(0.1)
            try:
                same = dev.identity(refresh=True)['device_id'] == ident['device_id']
            except osl.OslError:
                same = False
            print(f'0x{args.addr:02X} で応答しました' if same else f'0x{args.addr:02X} で応答がありません')

        if args.reset:
            ext = dev.ext_identity()
            if not ext or not ext['capability'] & osl.CAP_CMD_RESET:
                print('この機種はリセットコマンドに対応していません')
            else:
                print('リセット: ' + ('送信しました' if dev.reset() else '失敗'))
    finally:
        bus.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
