"""
OSL v2 子機のレジスタを任意の番地から 16 進ダンプするデバッグ用スクリプト。

usage:
    python dump_memory.py 0x10                    0x10 の子機の 0x0000 から 0x60 バイト
    python dump_memory.py 0x10 0x0300 0x20        番地 0x0300 から 0x20 バイト
    python dump_memory.py 0x10 --noaddr 0x20      番地を送らずに読む (読み出し位置の確認用)
    python dump_memory.py 0x10 --bridge cp2112
"""
import argparse
import sys

import osl

# 子機の名前などに cp932 で表せない文字があっても、コンソール表示で落ちないようにする
if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(errors='replace')


def hex_dump(data: bytes, base: int = 0x0000, width: int = 16):
    for i in range(0, len(data), width):
        chunk = data[i:i + width]
        hexs = ' '.join(f'{b:02X}' for b in chunk)
        text = ''.join(chr(b) if 0x20 <= b < 0x7F else '.' for b in chunk)
        print(f'{base + i:04X}: {hexs:<{width * 3}} {text}')


def main():
    ap = argparse.ArgumentParser(description='OSL v2 子機のレジスタを 16 進ダンプする')
    ap.add_argument('addr', type=lambda s: int(s, 0), help='子機の I2C アドレス')
    ap.add_argument('start', type=lambda s: int(s, 0), nargs='?', default=0x0000, help='先頭の番地')
    ap.add_argument('length', type=lambda s: int(s, 0), nargs='?', default=0x60, help='バイト数')
    ap.add_argument('--noaddr', type=lambda s: int(s, 0), metavar='LEN',
                    help='番地を送らずに LEN バイト読む')
    ap.add_argument('--bridge', choices=('auto', 'mcp2221', 'cp2112'), default='auto')
    ap.add_argument('--serial', help='ブリッジのシリアル番号 (複数接続時)')
    args = ap.parse_args()

    bus = osl.open_bridge(args.bridge, args.serial)
    if bus is None:
        print('USB-I2C ブリッジが見つかりません。')
        return 1
    try:
        if args.noaddr:
            data = bus.i2c_read(args.addr, args.noaddr)
            print(f'0x{args.addr:02X}: 番地なしで {args.noaddr} バイト')
            base = 0
        else:
            dev = osl.OslDevice(bus, args.addr)
            data = b''
            for ofs in range(0, args.length, 256):
                part = dev.read(args.start + ofs, min(256, args.length - ofs))
                if part is None:
                    data = None
                    break
                data += part
            print(f'0x{args.addr:02X}: 番地 0x{args.start:04X} から {args.length} バイト')
            base = args.start
        if data is None:
            print('読み出し失敗 (NACK / タイムアウト)')
            return 1
        hex_dump(data, base)
    finally:
        bus.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
