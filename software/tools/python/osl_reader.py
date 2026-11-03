"""
I2C バス上の OSL v2 子機 (温湿度・風速・絶対圧・差圧センサ) の情報を読み出す。

親機 (poem_pod) の代わりに Windows PC を I2C マスタにして、共通レジスタ v2 を直接読む。
子機単体の動作確認、アドレス・名前の確認、計測値の記録に使う。解釈は osl.py が行う。

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
import sys
import time

import osl

# 子機の名前などに cp932 で表せない文字があっても、コンソール表示で落ちないようにする
if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(errors='replace')

# 画面に出す計測値の小数点以下の桁数 (CSV には型の精度いっぱいで書く)
VALUE_DECIMALS = 2

UNIT_NAMES = osl.UNIT_NAMES


def read_info(dev: osl.OslDevice):
    """表示と記録に使う固定情報。読めなければ None"""
    try:
        ident = dev.identity()
        chs = dev.channels()
        return {
            'model_name': ident['model_name'],
            'device_id':  ident['device_id'],
            'name':       dev.name(),
            'count':      len(chs),
            'units':      [c['unit'] for c in chs],
            'unit_names': [c['unit_name'] for c in chs],
            'ch_names':   [c['name'] for c in chs],
        }
    except osl.OslError as e:
        print(f'[0x{dev.addr:02X}] {e}')
        return None


def format_values(info: dict, poll: dict) -> str:
    parts = []
    for i in range(info['count']):
        st = poll['status'][i]
        mark = '' if st == 0 else f' ({osl.STATUS_NAMES.get(st, st)})'
        parts.append(f'{info["ch_names"][i]} {poll["values"][i]:.{VALUE_DECIMALS}f} '
                     f'{info["unit_names"][i]}{mark}')
    return ', '.join(parts)


def show(dev: osl.OslDevice):
    info = read_info(dev)
    if info is None:
        return
    ident = dev.identity()
    print(f'[0x{dev.addr:02X}] {ident["model_name"]}')
    print(f'  Vendor    : {osl.VENDORS.get(ident["vendor"], hex(ident["vendor"]))}'
          f'  model 0x{ident["model"]:04X}  fw {ident["fw"]}  spec v{ident["spec"]}')
    print(f'  Device ID : {ident["device_id"]}')
    print(f'  Name      : "{info["name"]}"')
    print(f'  更新周期  : {ident["period_ms"]} ms')
    for i, ch in enumerate(dev.channels()):
        extra = []
        if ch['precision']:
            extra.append(f'有効桁 {ch["precision"]}')
        if ch['flags'] & 0x02:
            extra.append('補助値')
        if ch['flags'] & 0x01:
            extra.append('積算値')
        print(f'  ch{i}       : {ch["name"]} [{ch["unit_name"]}]'
              + (f'  ({", ".join(extra)})' if extra else ''))

    poll = dev.poll()
    if poll is None:
        print('  Values    : 読み出し失敗 (CRC 不一致)')
    else:
        print(f'  Values    : {format_values(info, poll)}   (sample_seq {poll["seq"]})')

    ext = dev.ext_identity()
    if ext:
        print(f'  拡張      : capability 0x{ext["capability"]:04X}  hw_rev {ext["hw_rev"]}'
              f'  平均 {ext["avg_window_ms"]} ms  予熱 {ext["warmup_ms"]} ms')
    try:
        st = dev.ext_status()
    except osl.OslError as e:
        st = None
        print(f'  状態      : {e}')
    if st:
        flags = []
        if st['node_flags'] & 0x01:
            flags.append('準備中')
        if st['node_flags'] & 0x02:
            flags.append('設定消失')
        print(f'  状態      : 稼働 {st["uptime_s"]} s  起動要因 {st["reset_cause_name"]}'
              f'  I2C異常 {st["err_i2c"]}  センサ異常 {st["err_sensor"]}'
              + (f'  [{", ".join(flags)}]' if flags else ''))

    try:
        user = dev.read_user()
    except osl.OslError as e:
        user = {}
        print(f'  ユーザー領域: {e}')
    if user:
        units = {n: u for n, _o, _f, u in dev.user_fields()}
        text = ', '.join(f'{k}={v:g}{units[k]}' if isinstance(v, float) else f'{k}={v}'
                         for k, v in user.items())
        print(f'  ユーザー領域: {text}')
    print()


def csv_path_for(base: str, addr: int) -> str:
    """子機ごとの CSV ファイル名。log.csv → log_0x11.csv"""
    stem, ext = os.path.splitext(base)
    return f'{stem}_0x{addr:02X}{ext or ".csv"}'


def open_csv(path: str, info):
    """1 台分の CSV を開いてヘッダを書き、(file, writer, 計測値の数) を返す。
    Excel で ° が化けないよう BOM 付き UTF-8 で書く。"""
    n_values = info['count'] if info else 0
    f = open(path, 'w', newline='', encoding='utf-8-sig')
    w = csv.writer(f)
    header = ['datetime', 'addr', 'device_id', 'name', 'model', 'result', 'sample_seq', 'delta_seq']
    for i in range(n_values):
        header += [f'value{i}', f'unit{i}', f'status{i}']
    w.writerow(header)
    return f, w, n_values


class Watcher:
    """子機を周期的に読み、コンソール表示と CSV 書き出しを行う。
    1 周期ぶんの処理を tick() にまとめてあり、テキスト表示 (watch) とグラフ表示
    (osl_plot.py) の両方から呼ぶ。"""

    def __init__(self, devs: list, csv_path: str):
        self.devs = devs
        self.addrs = [d.addr for d in devs]
        self.infos = {d.addr: read_info(d) for d in devs}
        self.last_seq = {}
        self.csv_path = csv_path
        self.csv_paths = {a: csv_path_for(csv_path, a) for a in self.addrs}
        self.files = {a: open_csv(self.csv_paths[a], self.infos[a]) for a in self.addrs}

    def tick(self) -> list:
        """全子機を 1 回ずつ読む。戻り値は [(addr, info, poll)]。poll は失敗で None"""
        now = datetime.datetime.now()
        stamp = now.strftime('%H:%M:%S')
        iso = now.isoformat(timespec='milliseconds')
        results = []
        for d in self.devs:
            a = d.addr
            info = self.infos[a]
            _f, w, n_values = self.files[a]
            poll = d.poll() if info else None
            if poll is None:
                print(f'{stamp} [0x{a:02X}] 読み出し失敗')
                w.writerow([iso, f'0x{a:02X}', info['device_id'] if info else '',
                            info['name'] if info else '', info['model_name'] if info else '',
                            'read_failed'])
                results.append((a, info, None))
                continue
            dseq = (poll['seq'] - self.last_seq[a]) % 65536 if a in self.last_seq else None
            self.last_seq[a] = poll['seq']
            dstr = f'Δseq={dseq:3d}' if dseq is not None else 'Δseq=  -'
            print(f'{stamp} [0x{a:02X}] {dstr}  {format_values(info, poll)}')

            row = [iso, f'0x{a:02X}', info['device_id'], info['name'], info['model_name'], 'ok',
                   poll['seq'], '' if dseq is None else dseq]
            for i in range(n_values):
                v = poll['values'][i]
                row += [f'{v:.7g}' if isinstance(v, float) else v,
                        info['unit_names'][i], poll['status'][i]]
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


def watch(devs: list, interval: float, csv_path: str):
    """計測値を interval 秒ごとに表示し、CSV にも書き出す"""
    watcher = Watcher(devs, csv_path)
    watcher.print_csv_paths()
    print('Ctrl+C で終了')
    # sleep(interval) だと読み出しにかかる時間ぶん周期が延びる。開始時刻からの格子に合わせる。
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
    ap = argparse.ArgumentParser(description='OSL v2 子機の情報を USB-I2C ブリッジ経由で読む')
    ap.add_argument('--bridge', choices=('auto', 'mcp2221', 'cp2112'), default='auto',
                    help='使う USB-I2C ブリッジ (既定 auto: MCP2221 → CP2112 の順に探す)')
    ap.add_argument('--serial', help='ブリッジのシリアル番号 (複数接続時)')
    ap.add_argument('--addr', type=lambda s: int(s, 0), help='子機の I2C アドレス (省略時は走査)')
    ap.add_argument('--watch', type=float, metavar='SEC',
                    help='SEC 秒ごとに計測値を表示し続け、CSV にも書き出す')
    ap.add_argument('--csv', metavar='PATH',
                    help='watch の CSV 出力先。子機ごとに _0xNN を付けた名前になる')
    ap.add_argument('--plot', action='store_true',
                    help='watch の計測値をグラフで表示する (--watch 省略時は 1 秒)')
    ap.add_argument('--span', type=float, default=600, metavar='SEC',
                    help='グラフに表示する期間 [秒] (既定 600)')
    args = ap.parse_args()
    if args.plot and not args.watch:
        args.watch = 1.0

    bus = osl.open_bridge(args.bridge, args.serial)
    if bus is None:
        print('USB-I2C ブリッジが見つかりません。')
        return 1

    try:
        if args.addr is not None:
            devs = [osl.OslDevice(bus, args.addr)]
        else:
            print('Scanning 0x08-0x77 ...')
            devs = [d for _a, d in osl.scan(bus)]
            if not devs:
                print('OSL v2 子機が見つかりません。配線・プルアップ・子機の電源・ファームウェアの版を確認してください。')
                return 1
            print(f'Found: {", ".join(f"0x{d.addr:02X}" for d in devs)}\n')

        for d in devs:
            show(d)

        if args.watch:
            csv_path = args.csv or datetime.datetime.now().strftime('osl_%Y%m%d_%H%M%S.csv')
            if args.plot:
                import osl_plot     # matplotlib はグラフを使うときだけ読み込む
                watcher = Watcher(devs, csv_path)
                watcher.print_csv_paths()
                try:
                    osl_plot.run(watcher, args.watch, args.span)
                finally:
                    watcher.close()
            else:
                watch(devs, args.watch, csv_path)
    finally:
        bus.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
