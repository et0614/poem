"""
OSL v2 子機の適合試験。共通レジスタ v2 の約束を一通り確かめ、結果を一覧で出す。

usage:
    python osl_v2_test.py 0x10                      0x10 の子機を試験する
    python osl_v2_test.py 0x10 --test-addr 0x11     アドレス変更も試す (0x11 に変えて 0x10 に戻す)
    python osl_v2_test.py 0x10 --bridge cp2112

試験は「読むだけ → 書いて元に戻す → リセット → アドレス変更」の順に進む。
名前とユーザー領域は試験の前の値に戻す。
"""
import argparse
import sys
import time

import osl

# 子機の名前などに cp932 で表せない文字があっても、コンソール表示で落ちないようにする
if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(errors='replace')

# EEPROM への保存を待つ時間 [s]。子機は 1 バイト約 10 ms で書く (名前 64 B で約 0.7 s)。
EEPROM_WAIT = 1.0


class Report:
    def __init__(self):
        self.rows = []

    def check(self, title: str, ok: bool, detail: str = ''):
        self.rows.append((title, ok, detail))
        mark = 'PASS' if ok else 'FAIL'
        print(f'  [{mark}] {title}' + (f' : {detail}' if detail else ''))
        return ok

    def skip(self, title: str, reason: str):
        self.rows.append((title, None, reason))
        print(f'  [SKIP] {title} : {reason}')

    def summary(self) -> int:
        n_pass = sum(1 for _t, ok, _d in self.rows if ok is True)
        n_fail = sum(1 for _t, ok, _d in self.rows if ok is False)
        n_skip = sum(1 for _t, ok, _d in self.rows if ok is None)
        print(f'\n結果: {n_pass} 合格 / {n_fail} 不合格 / {n_skip} 省略')
        return 0 if n_fail == 0 else 1


def section(title: str):
    print(f'\n■ {title}')


def test_read_only(bus, dev: osl.OslDevice, r: Report):
    a = dev.addr

    section('安全な走査と識別')
    b = bus.i2c_read(a, osl.IDENT_LEN)
    r.check('番地なしの読み出しで "OSL" が返る', b is not None and b[0:3] == b'OSL',
            '' if b is None else b[0:3].hex())
    r.check('識別区画の CRC', osl.check_crc(b))
    ident = dev.identity()
    r.check('仕様版 = 2', ident['spec'] == 2, str(ident['spec']))
    r.check('チャネル数が 1〜16', 1 <= ident['count'] <= 16, str(ident['count']))
    r.check('device_id がすべて 0 ではない', any(bytes.fromhex(ident['device_id'])), ident['device_id'])
    print(f'    {ident["model_name"]}  model 0x{ident["model"]:04X}  fw {ident["fw"]}'
          f'  周期 {ident["period_ms"]} ms')

    section('各区画の CRC')
    n = ident['count']
    r.check('名前の CRC', osl.check_crc(dev.read(osl.REG_NAME, osl.NAME_LEN + 2)))
    r.check('チャネル記述の CRC', osl.check_crc(dev.read(osl.REG_CHDESC, 16 * n + 2)))
    ext_raw = dev.read(osl.REG_EXT_IDENT, 10)
    if osl.is_unimplemented(ext_raw):
        r.skip('拡張識別の CRC', '任意区画は未実装 (すべて 0)')
    else:
        r.check('拡張識別の CRC', osl.check_crc(ext_raw))
    ext = dev.ext_identity()
    if ext and ext['capability'] & osl.CAP_CH_ATTR:
        r.check('拡張チャネル属性の CRC', osl.check_crc(dev.read(osl.REG_EXT_ATTR, 4 * n + 2)))
    if ext and ext['capability'] & osl.CAP_EXT_STATUS:
        r.check('拡張ステータスの CRC', osl.check_crc(dev.read(osl.REG_EXT_STATUS, 12)))
    for ch in dev.channels():
        print(f'    ch: {ch["name"]} [{ch["unit_name"]}] type {ch["type"]}'
              f' 有効桁 {ch["precision"]} flags 0x{ch["flags"]:02X}')

    section('未実装・予約の番地')
    for reg, ln in [(0x0022, 30), (0x0082, 32), (0x0200, 32), (0x0354, 16), (0x0F00, 16)]:
        b = dev.read(reg, ln)
        r.check(f'0x{reg:04X} から {ln} B が 0', b is not None and not any(b),
                '' if b is None else b[:8].hex())

    section('読み取り専用の区画への書き込み')
    before = dev.read(osl.REG_IDENT, osl.IDENT_LEN)
    dev.write(osl.REG_IDENT, b'XYZ')
    after = dev.read(osl.REG_IDENT, osl.IDENT_LEN)
    r.check('識別区画は書き換わらない', before == after)

    section('読み出し位置の規則 (仕様 §1.2)')
    chdesc = dev.read(osl.REG_CHDESC, 16)
    ok = bus.i2c_write(a, [osl.REG_CHDESC >> 8, osl.REG_CHDESC & 0xFF])     # 番地だけ書いて STOP
    b = bus.i2c_read(a, 16) if ok else None                                   # 改めて読む
    r.check('番地だけ書いて STOP → 読むと、その番地から返る', b == chdesc)
    b = bus.i2c_read(a, osl.IDENT_LEN)
    r.check('読み出しの後は 0x0000 に戻る', b is not None and b[0:3] == b'OSL')
    dev.write(0x0060, [0x00])                                                 # データ付きの書き込み
    b = bus.i2c_read(a, osl.IDENT_LEN)
    r.check('データ付きの書き込みの後は 0x0000 に戻る', b is not None and b[0:3] == b'OSL')

    section('POLL')
    polls, bad = [], 0
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < 2.0:
        p = dev.poll(tries=1)
        if p is None:
            bad += 1
        else:
            polls.append((time.perf_counter(), p))
    r.check('連続して読んでも CRC が合う (値が裂けない)', bad == 0,
            f'{len(polls)} 回中 失敗 {bad} 回')
    if len(polls) >= 2:
        (ta, pa), (tb, pb) = polls[0], polls[-1]
        dseq = (pb['seq'] - pa['seq']) % 65536
        expect = (tb - ta) * 1000 / ident['period_ms']
        r.check('sample_seq が更新周期どおりに進む', abs(dseq - expect) <= max(2, expect * 0.2),
                f'{dseq} 進んだ (期待 {expect:.1f})')
        p = pb
        vals = ', '.join(f'{v:.4g}' for v in p['values'])
        print(f'    値: {vals}  状態: {p["status"]}')
        r.check('全チャネルの状態が正常', all(s == 0 for s in p['status']), str(p['status']))

    if ext and ext['capability'] & osl.CAP_EXT_STATUS:
        section('拡張ステータス')
        s1 = dev.ext_status()
        time.sleep(2.2)
        s2 = dev.ext_status()
        r.check('稼働時間が進む', 1 <= s2['uptime_s'] - s1['uptime_s'] <= 4,
                f'{s1["uptime_s"]} → {s2["uptime_s"]} s')
        print(f'    起動要因 {s2["reset_cause_name"]}  node_flags 0x{s2["node_flags"]:02X}'
              f'  I2C異常 {s2["err_i2c"]}  センサ異常 {s2["err_sensor"]}')


def test_writes(dev: osl.OslDevice, r: Report):
    section('名前の書き込み')
    original = dev.name()
    test_name = 'テスト 3F 会議室 天井 Ω-01'
    ok = dev.set_name(test_name)
    time.sleep(EEPROM_WAIT)
    raw = dev.read(osl.REG_NAME, osl.NAME_LEN + 2)
    r.check('UTF-8 の名前を書いて読み戻せる', ok and dev.name() == test_name, f'"{dev.name()}"')
    r.check('名前の CRC が作り直される', osl.check_crc(raw))
    long_name = 'あ' * 21    # 63 バイト (上限)
    ok = dev.set_name(long_name)
    time.sleep(EEPROM_WAIT)
    r.check('63 バイトの名前 (上限)', ok and dev.name() == long_name)
    dev.set_name(original)
    time.sleep(EEPROM_WAIT)
    r.check('元の名前に戻した', dev.name() == original, f'"{original}"')

    section('ユーザー領域')
    fields = [f for f in dev.user_fields() or [] if f[2] == 'f']
    if not fields:
        r.skip('ユーザー領域の読み書き', 'この機種の項目が osl.py に無い')
        return
    name = fields[-1][0]
    before = dev.read_user()[name]
    dev.write_user(name, before + 0.5)
    time.sleep(0.2)
    r.check(f'{name} を書いて読み戻せる', abs(dev.read_user()[name] - (before + 0.5)) < 1e-6)
    dev.write_user(name, before)
    time.sleep(0.2)
    r.check(f'{name} を元に戻した', dev.read_user()[name] == before, f'{before:g}')


def test_reset(dev: osl.OslDevice, r: Report):
    ext = dev.ext_identity()
    if not ext or not ext['capability'] & osl.CAP_CMD_RESET:
        r.skip('リセット', 'リセットコマンドに対応していない')
        return
    section('リセットと設定の保持')
    name = dev.name()
    user = dev.read_user()
    s1 = dev.ext_status()
    r.check('リセットコマンドを送れる', dev.reset())
    time.sleep(1.5)
    try:
        s2 = dev.ext_status()
    except osl.OslError as e:
        r.check('リセット後に応答する', False, str(e))
        return
    r.check('稼働時間が巻き戻る', s2['uptime_s'] < s1['uptime_s'] or s1['uptime_s'] < 2,
            f'{s1["uptime_s"]} → {s2["uptime_s"]} s')
    r.check('起動要因 = ソフトウェア', s2['reset_cause'] == 4, s2['reset_cause_name'])
    r.check('名前が保持される', dev.name() == name)
    r.check('ユーザー領域が保持される', dev.read_user() == user)
    r.check('設定消失の印が無い', not s2['node_flags'] & 0x02, f'0x{s2["node_flags"]:02X}')


def test_address(bus, dev: osl.OslDevice, new_addr: int, r: Report):
    section(f'アドレス変更 (0x{dev.addr:02X} → 0x{new_addr:02X} → 戻す)')
    if bus.probe(new_addr):
        r.skip('アドレス変更', f'0x{new_addr:02X} に別の機器がいる')
        return
    old = dev.addr
    did = dev.identity()['device_id']

    other = osl.OslDevice(bus, old)
    other.write(osl.REG_ADDR_KEY + 1, [new_addr])       # 鍵なしで新アドレスだけ書く
    time.sleep(0.1)
    r.check('鍵が無ければ変わらない', bus.probe(old) and not bus.probe(new_addr))

    ok = dev.set_address(new_addr)
    time.sleep(0.2)
    r.check('鍵付きで変えると新アドレスで応答する',
            ok and dev.identity(refresh=True)['device_id'] == did)
    r.check('旧アドレスでは応答しない', not bus.probe(old))

    dev.set_address(old)
    time.sleep(0.2)
    r.check('元のアドレスに戻した', dev.identity(refresh=True)['device_id'] == did)


def main():
    ap = argparse.ArgumentParser(description='OSL v2 子機の適合試験')
    ap.add_argument('addr', type=lambda s: int(s, 0), help='子機の I2C アドレス')
    ap.add_argument('--test-addr', type=lambda s: int(s, 0), metavar='ADDR',
                    help='アドレス変更の試験に使う一時的なアドレス')
    ap.add_argument('--bridge', choices=('auto', 'mcp2221', 'cp2112'), default='auto')
    ap.add_argument('--serial', help='ブリッジのシリアル番号 (複数接続時)')
    args = ap.parse_args()

    bus = osl.open_bridge(args.bridge, args.serial)
    if bus is None:
        print('USB-I2C ブリッジが見つかりません。')
        return 1
    r = Report()
    try:
        dev = osl.OslDevice(bus, args.addr)
        try:
            dev.identity()
        except osl.OslError as e:
            print(e)
            return 1
        test_read_only(bus, dev, r)
        test_writes(dev, r)
        test_reset(dev, r)
        if args.test_addr is not None:
            test_address(bus, dev, args.test_addr, r)
    finally:
        bus.close()
    return r.summary()


if __name__ == '__main__':
    sys.exit(main())
