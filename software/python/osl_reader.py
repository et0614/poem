"""
I2C バス上の OSL 子機 (温湿度・風速・絶対圧・差圧センサ) の情報を読み出す。

親機 (poem_pod) の代わりに Windows PC を I2C マスタにして、共通レジスタ
(SHASE 2026 第2報) を直接読む。子機単体の動作確認やアドレス・名前の確認用。

USB-I2C ブリッジは MCP2221 と CP2112 のどちらでも使える。接続されている方を自動で選ぶ。

usage:
    python osl_reader.py                  バスを走査し、見つかった子機をすべて表示
    python osl_reader.py --addr 0x10      指定アドレスの子機だけ表示
    python osl_reader.py --watch 1        1 秒ごとに計測値を表示し続ける (Ctrl+C で終了)
                                          CSV (osl_YYYYMMDD_HHMMSS_0xNN.csv、子機ごと) にも書き出す
    python osl_reader.py --watch 1 --csv log.csv   CSV のファイル名を指定する (log_0xNN.csv になる)
    python osl_reader.py --watch 1 --plot          グラフでも表示する (osl_plot.py)
    python osl_reader.py --watch 1 --plot --span 3600   グラフの横軸を直近 1 時間にする
    python osl_reader.py --bridge cp2112  CP2112 に固定する (既定は auto)
    python osl_reader.py --serial XXXX    ブリッジが複数あるときにシリアル番号で選ぶ
"""
import argparse
import csv
import datetime
import os
import struct
import sys
import time

# ===== 共通レジスタ (firmware/*/i2c_shared_data.h と同期) ==================
REG_INFO       = 0x00   # INFO BLOCK 0x00-0x27 (40 byte)
INFO_LEN       = 0x28
REG_POLL       = 0x28   # POLL BLOCK 0x28-0x4B (36 byte)
POLL_LEN       = 0x24
REG_EXTENSION  = 0x4C
NAME_LEN       = 16

# 画面に出す計測値の小数点以下の桁数 (CSV には float の精度いっぱいで書く)
VALUE_DECIMALS = 2

# BACnet Engineering Units (ASHRAE 135) のうち子機が使うもの
UNIT_NAMES = {
    5:  'V',
    29: '%RH',
    53: 'Pa',
    62: '°C',
    74: 'm/s',
}

# unit_type[0..1] の組から機種を推定する。
# 拡張領域が「value[0] の a, b → value[1] の a, b」の float 4 個である機種は
# coef_labels を持つ。風速センサは拡張領域の形が違うので表示しない。
SENSOR_TYPES = {
    (62, 29): ('温湿度センサ (SHT4x)', ('温度', '湿度')),
    (53, 62): ('圧力センサ', ('圧力', '温度')),
    (74, 5):  ('風速センサ', None),
}

# 絶対圧 (BMP585) と差圧 (SDP810) はレジスタも単位も同じで区別する情報が無い。
# 測定範囲が重ならない (BMP585 は 30〜125 kPa、SDP810-125Pa は ±125 Pa) ので
# value[0] の大きさで見分ける。起動直後などで値が無効なときは見分けない。
ABS_PRESSURE_MIN_PA = 10000.0


def bridge_classes(kind: str) -> list:
    """候補のブリッジクラスを探す順に返す"""
    from mcp2221_driver import MCP2221Device
    from cp2112_driver import CP2112Device
    if kind == 'mcp2221':
        return [MCP2221Device]
    if kind == 'cp2112':
        return [CP2112Device]
    # auto: 両方つながっていたら MCP2221 を優先する
    return [MCP2221Device, CP2112Device]


def open_bridge(kind: str, serial):
    """接続されているブリッジを探して開く。slave_addr は後から書き換えて使う。
    serial を指定したときは、そのシリアル番号を持つものだけを選ぶ。"""
    for Bridge in bridge_classes(kind):
        for d in Bridge.list_devices():
            if serial is not None and d['serial'] != serial:
                continue
            name = Bridge.__name__.replace('Device', '')
            print(f'Bridge: {name} - {d["product"]} (S/N: {d["serial"] or "-"})')
            dev = Bridge(slave_addr=0x00, serial_number=d['serial'] or None)
            return dev if dev.open() else None

    target = 'MCP2221 / CP2112' if kind == 'auto' else kind.upper()
    print(f'No {target} devices found'
          + (f' (serial "{serial}").' if serial is not None else '.'))
    return None


def scan(dev) -> list:
    """0x08-0x77 のうち、レジスタ 0x00 を読めたアドレスを返す"""
    found = []
    for addr in range(0x08, 0x78):
        dev.slave_addr = addr
        if dev.read_i2c_block(REG_INFO, 1, retries=0) is not None:
            found.append(addr)
    return found


def read_info(dev) -> dict:
    """INFO BLOCK を読んで辞書にする。読めなければ None"""
    raw = dev.read_i2c_block(REG_INFO, INFO_LEN)
    if raw is None:
        return None
    device_id, _key, new_addr, count = struct.unpack_from('<IBBB', raw, 0x00)
    units = list(struct.unpack_from('<8H', raw, 0x08))
    name_raw = raw[0x18:0x18 + NAME_LEN]
    if name_raw[:1] == b'\xff':
        name = ''
    else:
        name = name_raw.split(b'\x00', 1)[0].decode('ascii', errors='replace')
    return {'id': device_id, 'addr': new_addr, 'count': min(count, 8),
            'units': units, 'name': name}


def read_poll(dev) -> dict:
    """POLL BLOCK を読んで辞書にする。読めなければ None"""
    raw = dev.read_i2c_block(REG_POLL, POLL_LEN)
    if raw is None:
        return None
    status1, status2, meas_count, err_count = struct.unpack_from('<BBBB', raw, 0)
    values = list(struct.unpack_from('<8f', raw, 4))
    return {'st1': status1, 'st2': status2, 'sq': meas_count,
            'err': err_count, 'values': values}


def sensor_type(info: dict, poll: dict = None):
    """(機種名, 補正係数のラベル) を返す"""
    name, coef_labels = SENSOR_TYPES.get(tuple(info['units'][:2]), ('不明な子機', None))
    if name == '圧力センサ' and poll is not None and not (poll['st1'] & 1):
        if abs(poll['values'][0]) >= ABS_PRESSURE_MIN_PA:
            name = '絶対圧センサ (BMP585)'
            coef_labels = ('気圧', '温度')
        else:
            name = '差圧センサ (SDP810)'
            coef_labels = ('差圧', '温度')
    return name, coef_labels


def format_values(info: dict, poll: dict) -> str:
    parts = []
    for i in range(info['count']):
        unit = UNIT_NAMES.get(info['units'][i], f'unit{info["units"][i]}')
        stale = ' (stale)' if (poll['st1'] >> i) & 1 else ''
        parts.append(f'{poll["values"][i]:.{VALUE_DECIMALS}f} {unit}{stale}')
    return ', '.join(parts)


def show(dev, addr: int):
    dev.slave_addr = addr
    info = read_info(dev)
    poll = read_poll(dev)
    if info is None or poll is None:
        print(f'[0x{addr:02X}] 読み出し失敗')
        return

    type_name, coef_labels = sensor_type(info, poll)
    print(f'[0x{addr:02X}] {type_name}')
    print(f'  Device ID : {info["id"]} (0x{info["id"]:06X})')
    print(f'  Name      : "{info["name"]}"')
    print(f'  Data count: {info["count"]}  units={info["units"][:info["count"]]}')
    print(f'  Status1   : 0x{poll["st1"]:02X}'
          + ('  (全計測値が無効)' if poll['st1'] == 0xFF else ''))
    print(f'  meas_count: {poll["sq"]}   err_count: {poll["err"]}')
    print(f'  Values    : {format_values(info, poll)}')

    if coef_labels:
        raw = dev.read_i2c_block(REG_EXTENSION, 16)
        if raw is not None:
            a0, b0, a1, b1 = struct.unpack('<4f', raw)
            print(f'  補正係数  : {coef_labels[0]} y = {a0:g} x + {b0:g}')
            print(f'              {coef_labels[1]} y = {a1:g} x + {b1:g}')
    print()


def csv_path_for(base: str, addr: int) -> str:
    """子機ごとの CSV ファイル名。log.csv → log_0x11.csv"""
    stem, ext = os.path.splitext(base)
    return f'{stem}_0x{addr:02X}{ext or ".csv"}'


def open_csv(path: str, info):
    """1 台分の CSV を開いてヘッダを書き、(file, writer, 計測値の数) を返す。

    1 行 = 1 回の読み出し。value_i と unit_i を対にして並べる。
    Excel で ° が化けないよう BOM 付き UTF-8 で書く。
    """
    n_values = info['count'] if info else 0
    f = open(path, 'w', newline='', encoding='utf-8-sig')
    w = csv.writer(f)
    header = ['datetime', 'addr', 'device_id', 'name', 'sensor', 'result',
              'status1', 'meas_count', 'delta_sq', 'err_count']
    for i in range(n_values):
        header += [f'value{i}', f'unit{i}']
    w.writerow(header)
    return f, w, n_values


class Watcher:
    """子機を周期的に読み、コンソール表示と CSV 書き出しを行う。
    CSV は子機ごとに別ファイル (csv_path の名前に _0xNN を付けたもの) にする。

    1 周期ぶんの処理を tick() にまとめてあり、テキスト表示 (watch) とグラフ表示
    (osl_plot.py) の両方から呼ぶ。
    """

    def __init__(self, dev, addrs: list, csv_path: str):
        self.dev = dev
        self.addrs = addrs
        self.infos = {}
        for a in addrs:
            dev.slave_addr = a
            self.infos[a] = read_info(dev)
        self.last_sq = {}
        self.csv_path = csv_path
        self.csv_paths = {a: csv_path_for(csv_path, a) for a in addrs}
        self.files = {}     # addr -> (file, writer, 計測値の数)
        for a in addrs:
            self.files[a] = open_csv(self.csv_paths[a], self.infos[a])

    def tick(self) -> list:
        """全子機を 1 回ずつ読む。戻り値は [(addr, info, poll)]。poll は失敗で None"""
        now = datetime.datetime.now()
        stamp = now.strftime('%H:%M:%S')
        iso = now.isoformat(timespec='milliseconds')
        results = []
        for a in self.addrs:
            info = self.infos[a]
            _f, w, n_values = self.files[a]
            self.dev.slave_addr = a
            poll = read_poll(self.dev)
            if info is None or poll is None:
                print(f'{stamp} [0x{a:02X}] 読み出し失敗')
                w.writerow([iso, f'0x{a:02X}',
                                 info['id'] if info else '', info['name'] if info else '',
                                 sensor_type(info)[0] if info else '', 'read_failed'])
                results.append((a, info, None))
                continue
            dsq = (poll['sq'] - self.last_sq[a]) % 256 if a in self.last_sq else None
            self.last_sq[a] = poll['sq']
            dsq_str = f'Δsq={dsq:3d}' if dsq is not None else 'Δsq=  -'
            print(f'{stamp} [0x{a:02X}] st=0x{poll["st1"]:02X} {dsq_str}  '
                  f'{format_values(info, poll)}')

            row = [iso, f'0x{a:02X}', info['id'], info['name'],
                   sensor_type(info, poll)[0], 'ok',
                   poll['st1'], poll['sq'], '' if dsq is None else dsq, poll['err']]
            for i in range(n_values):
                # float32 の有効桁 (約 7 桁) で書く。stale かどうかは status1 の bit i で判断
                row += [f'{poll["values"][i]:.7g}',
                        UNIT_NAMES.get(info['units'][i], info['units'][i])]
            w.writerow(row)
            results.append((a, info, poll))
        # 途中で落ちても読めるよう、毎周期ディスクへ書き出す
        for f, _w, _n in self.files.values():
            f.flush()
        return results

    def print_csv_paths(self):
        for a in self.addrs:
            print(f'CSV: {self.csv_paths[a]}')

    def close(self):
        for f, _w, _n in self.files.values():
            f.close()


def watch(dev, addrs: list, interval: float, csv_path: str):
    """計測値を interval 秒ごとに表示し、CSV にも書き出す。meas_count の進み (Δsq) も出す"""
    watcher = Watcher(dev, addrs, csv_path)
    watcher.print_csv_paths()
    print('Ctrl+C で終了')
    # sleep(interval) だと読み出しにかかる時間ぶん周期が延び、Δsq がときどき 1 多く出る。
    # 開始時刻からの一定間隔の格子に合わせて待つ。
    next_t = time.perf_counter()
    try:
        while True:
            watcher.tick()
            next_t += interval
            time.sleep(max(0.0, next_t - time.perf_counter()))
    except KeyboardInterrupt:
        print()
    finally:
        watcher.close()


def main():
    ap = argparse.ArgumentParser(description='OSL 子機の情報を USB-I2C ブリッジ経由で読む')
    ap.add_argument('--bridge', choices=('auto', 'mcp2221', 'cp2112'), default='auto',
                    help='使う USB-I2C ブリッジ (既定 auto: MCP2221 → CP2112 の順に探す)')
    ap.add_argument('--serial', help='ブリッジのシリアル番号 (複数接続時)')
    ap.add_argument('--addr', type=lambda s: int(s, 0), help='子機の I2C アドレス (省略時は走査)')
    ap.add_argument('--watch', type=float, metavar='SEC',
                    help='SEC 秒ごとに計測値を表示し続け、CSV にも書き出す')
    ap.add_argument('--csv', metavar='PATH',
                    help='watch の CSV 出力先。子機ごとに _0xNN を付けた名前になる (既定: osl_YYYYMMDD_HHMMSS.csv)')
    ap.add_argument('--plot', action='store_true',
                    help='watch の計測値をグラフで表示する (--watch 省略時は 1 秒)')
    ap.add_argument('--span', type=float, default=600, metavar='SEC',
                    help='グラフに表示する期間 [秒] (既定 600)')
    args = ap.parse_args()
    if args.plot and not args.watch:
        args.watch = 1.0

    dev = open_bridge(args.bridge, args.serial)
    if dev is None:
        return 1

    try:
        if args.addr is not None:
            addrs = [args.addr]
        else:
            print('Scanning 0x08-0x77 ...')
            addrs = scan(dev)
            if not addrs:
                print('子機が見つかりません。配線・プルアップ・子機の電源を確認してください。')
                return 1
            print(f'Found: {", ".join(f"0x{a:02X}" for a in addrs)}\n')

        for a in addrs:
            show(dev, a)

        if args.watch:
            csv_path = args.csv or datetime.datetime.now().strftime('osl_%Y%m%d_%H%M%S.csv')
            if args.plot:
                # matplotlib はグラフを使うときだけ読み込む (テキスト表示だけなら不要)
                import osl_plot
                watcher = Watcher(dev, addrs, csv_path)
                watcher.print_csv_paths()
                try:
                    osl_plot.run(watcher, args.watch, args.span)
                finally:
                    watcher.close()
            else:
                watch(dev, addrs, args.watch, csv_path)
    finally:
        dev.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
