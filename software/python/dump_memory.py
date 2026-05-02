"""
子機 (温湿度センサ / 風速センサ) の I2C 共通レジスタを 0x00 から
任意バイトダンプするデバッグスクリプト。

usage:
    python dump_memory.py [serial] [slave_addr_hex] [length_hex]
    例: python dump_memory.py             -> 0x10, 0x60 byte
        python dump_memory.py - 0x10 0x60
"""
import sys
from cp2112_driver import CP2112Device


def hex_dump(data: bytes, base: int = 0x00, width: int = 16):
    for row in range(0, len(data), width):
        chunk = data[row:row + width]
        hex_part = ' '.join(f'{b:02X}' for b in chunk)
        ascii_part = ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)
        print(f'  {base + row:02X}: {hex_part:<{width * 3}}  {ascii_part}')


def main():
    serial = sys.argv[1] if len(sys.argv) > 1 and sys.argv[1] != '-' else None
    addr   = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x10
    length = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x60

    print(f'Target: addr=0x{addr:02X}, length={length} ({length:#x})')
    if serial:
        print(f'CP2112 serial: {serial}')

    with CP2112Device(slave_addr=addr, serial_number=serial) as dev:
        if not dev.is_open:
            print('Could not open CP2112.')
            return 1

        # 1) 一括読み出し
        print('\n--- Block read (offset=0x00, length above) ---')
        block = dev.read_i2c_block(0x00, length)
        if block:
            hex_dump(block, base=0x00)
        else:
            print('Block read failed (NACK/timeout?).')

        # 2) 1 バイトずつの読み出しと比較 (register pointer 動作確認用)
        print('\n--- Per-byte read (offset 0x00..0x1F) ---')
        per_byte = bytearray()
        for off in range(min(length, 0x20)):
            r = dev.read_i2c_block(off, 1)
            per_byte.append(r[0] if r else 0xEE)  # 0xEE = read failed
        hex_dump(bytes(per_byte), base=0x00)

        # 3) 一括読みとの差分
        if block:
            print('\n--- Diff (block vs per-byte, first 0x20) ---')
            mismatches = []
            for i in range(min(0x20, len(block))):
                if block[i] != per_byte[i]:
                    mismatches.append((i, block[i], per_byte[i]))
            if not mismatches:
                print('  no diff (slave register-pointer behaves consistently)')
            else:
                for off, b, p in mismatches:
                    print(f'  0x{off:02X}: block=0x{b:02X}  per-byte=0x{p:02X}')

    return 0


if __name__ == '__main__':
    sys.exit(main())
