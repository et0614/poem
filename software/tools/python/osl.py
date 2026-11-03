"""
OSL 共通レジスタ v2 の解釈層 (PoEM Link 通信仕様の L4)。

子機のレジスタを読み書きして、識別・名前・チャネル・計測値 (POLL)・拡張区画を
扱う。仕様は「OSL共通レジスタv2_アドレス表.md」。

I2C の通信手段 (L3) は「バス」として外から渡す。バスに求めるのは次の 4 つだけ:
    probe(addr) -> bool                          機器がいるか (書き込みはしない)
    i2c_read(addr, n) -> bytes | None            番地を送らない読み出し
    i2c_write(addr, data) -> bool                書き込み
    i2c_write_read(addr, wdata, n) -> bytes | None  書いてから Repeated START で読む
    MAX_WRITE (int)                              1 回の書き込みの上限
MCP2221Device / CP2112Device がこれを持つ。PoEM Link 用のバスも同じ形で作る。
"""
import struct

# <editor-fold desc="番地 (仕様 §3, §4)">

REG_IDENT      = 0x0000     # 識別 32 B (CRC 込み)
REG_ADDR_KEY   = 0x0020
REG_NAME       = 0x0040     # 名前 64 B + CRC 2 B
REG_CHDESC     = 0x0100     # チャネル記述 16 B × N + CRC
REG_POLL       = 0x0300     # POLL
REG_EXT_IDENT  = 0x0400     # 拡張識別 8 B + CRC
REG_EXT_ATTR   = 0x0420     # 拡張チャネル属性 4 B × N + CRC
REG_EXT_STATUS = 0x0480     # 拡張ステータス 10 B + CRC
REG_CMD_KEY    = 0x04C0
REG_USER       = 0x1000     # ユーザー領域

IDENT_LEN = 32
NAME_LEN  = 64
KEY       = 0xA5

CAP_CH_ATTR      = 0x0001
CAP_EXT_STATUS   = 0x0002
CAP_CMD_RESET    = 0x0004
CAP_CMD_IDENTIFY = 0x0008

TYPE_FLOAT32, TYPE_INT32, TYPE_UINT32 = 0, 1, 2
_TYPE_FMT = {TYPE_FLOAT32: '<f', TYPE_INT32: '<i', TYPE_UINT32: '<I'}

# </editor-fold>

# <editor-fold desc="表 (仕様 §6)">

# BACnet Engineering Units のうち表示に使うもの
UNIT_NAMES = {5: 'V', 29: '%RH', 53: 'Pa', 62: '°C', 74: 'm/s', 95: ''}

# status[i] (BACnet Reliability)
STATUS_NAMES = {
    0: '正常', 1: 'センサ応答なし', 2: '上限超過', 3: '下限未満',
    7: '異常', 12: '通信失敗', 64: '準備中',
}

RESET_CAUSES = {0: '不明', 1: '電源投入', 2: '電圧低下', 3: 'ウォッチドッグ',
                4: 'ソフトウェア', 5: '外部リセット', 6: 'その他'}

VENDORS = {0x0001: 'PoEM'}

# model_id → (表示名, ユーザー領域の項目)。項目は (名前, オフセット, 型, 単位)。
# ユーザー領域の形は機種ごとに違う (仕様 §5)。ここに無い機種は生のバイトで扱う。
_COEF4 = lambda c0, u0, c1, u1: [
    (f'{c0}_a', 0x00, 'f', ''), (f'{c0}_b', 0x04, 'f', u0),
    (f'{c1}_a', 0x08, 'f', ''), (f'{c1}_b', 0x0C, 'f', u1),
]
MODELS = {
    0x0101: ('温湿度センサ (SHT4x)',        _COEF4('temp', '°C', 'humi', '%RH')),
    0x0102: ('多点温度センサ (TMP117×3、FPC)', []),
    0x0201: ('風速センサ (熱線式)',         [('enable', 0x00, 'B', ''), ('filter_n', 0x01, 'B', ''),
                                             ('E0', 0x04, 'f', 'V'), ('m1', 0x08, 'f', ''),
                                             ('lnC1', 0x0C, 'f', ''), ('m2', 0x10, 'f', ''),
                                             ('lnC2', 0x14, 'f', ''), ('m3', 0x18, 'f', ''),
                                             ('lnC3', 0x1C, 'f', ''), ('vSplit1', 0x20, 'f', 'm/s'),
                                             ('vSplit2', 0x24, 'f', 'm/s')]),
    0x0301: ('絶対圧センサ (BMP585)',       _COEF4('press', 'Pa', 'temp', '°C')),
    0x0302: ('差圧センサ (SDP810-125Pa)',   _COEF4('dp', 'Pa', 'temp', '°C')),
    0x0303: ('差圧センサ (SDP810-500Pa)',   _COEF4('dp', 'Pa', 'temp', '°C')),
}

# </editor-fold>


def crc16(data) -> int:
    """CRC-16/CCITT-FALSE (多項式 0x1021、初期値 0xFFFF、反転なし)。"123456789" → 0x29B1"""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def check_crc(block: bytes) -> bool:
    """末尾 2 B (LE) が先頭からの CRC と一致するか"""
    if block is None or len(block) < 3:
        return False
    return crc16(block[:-2]) == struct.unpack_from('<H', block, len(block) - 2)[0]


def is_unimplemented(block: bytes) -> bool:
    """任意区画が未実装 (CRC も含めてすべて 0) か (仕様 §1.3)"""
    return block is not None and not any(block)


def _cstr(raw: bytes) -> str:
    return raw.split(b'\x00', 1)[0].decode('utf-8', errors='replace')


class OslError(Exception):
    pass


class OslDevice:
    """1 台の OSL v2 子機"""

    def __init__(self, bus, addr: int):
        self.bus = bus
        self.addr = addr
        self._ident = None
        self._channels = None
        self._ext = None

    # <editor-fold desc="低レベル">

    def read(self, reg: int, n: int, retries=2):
        """番地 reg から n バイト読む (16bit 番地は上位→下位で送る)"""
        return self.bus.i2c_write_read(self.addr, [reg >> 8, reg & 0xFF], n, retries=retries)

    def write(self, reg: int, data, retries=2) -> bool:
        return self.bus.i2c_write(self.addr, [reg >> 8, reg & 0xFF] + list(data), retries=retries)

    def read_checked(self, reg: int, n: int, tries=3) -> bytes:
        """CRC 込みの区画を読み、一致するまで読み直す"""
        for _ in range(tries):
            b = self.read(reg, n)
            if check_crc(b):
                return b
        raise OslError(f'0x{self.addr:02X} 0x{reg:04X}: CRC 不一致または読み出し失敗')

    # </editor-fold>

    # <editor-fold desc="必須区画">

    def identity(self, refresh=False) -> dict:
        if self._ident is None or refresh:
            b = self.read_checked(REG_IDENT, IDENT_LEN)
            if b[0:3] != b'OSL':
                raise OslError(f'0x{self.addr:02X}: OSL 子機ではない')
            vendor, model = struct.unpack_from('<HH', b, 4)
            period, = struct.unpack_from('<H', b, 12)
            self._ident = {
                'spec': b[3], 'vendor': vendor, 'model': model,
                'fw': f'{b[8]}.{b[9]}.{b[10]}', 'count': b[11],
                'period_ms': period, 'device_id': b[14:30].hex().upper(),
                'model_name': MODELS.get(model, (f'不明な機種 0x{model:04X}', None))[0],
            }
        return self._ident

    def name(self) -> str:
        return _cstr(self.read_checked(REG_NAME, NAME_LEN + 2)[:NAME_LEN])

    def set_name(self, text: str) -> bool:
        """名前を書く (UTF-8、63 バイトまで)。64 バイトを 1 回で書けないバスでは分けて書く"""
        raw = text.encode('utf-8')
        if len(raw) > NAME_LEN - 1:
            raise ValueError(f'名前は UTF-8 で {NAME_LEN - 1} バイトまで (現在 {len(raw)} バイト)')
        raw = raw + bytes(NAME_LEN - len(raw))
        step = self.bus.MAX_WRITE - 2
        if step >= NAME_LEN:
            return self.write(REG_NAME, raw)
        # 分けて書くと、途中の取引の STOP で一旦反映される (仕様は 1 回での書き込みを推奨)
        return all(self.write(REG_NAME + i, raw[i:i + step]) for i in range(0, NAME_LEN, step))

    def channels(self, refresh=False) -> list:
        if self._channels is None or refresh:
            n = self.identity()['count']
            b = self.read_checked(REG_CHDESC, 16 * n + 2)
            chs = []
            for i in range(n):
                unit, vtype = struct.unpack_from('<HB', b, 16 * i)
                chs.append({'unit': unit, 'type': vtype,
                            'name': _cstr(b[16 * i + 4:16 * i + 16]),
                            'unit_name': UNIT_NAMES.get(unit, f'unit{unit}'),
                            'precision': 0, 'flags': 0})
            ext = self.ext_identity()
            if ext and ext['capability'] & CAP_CH_ATTR:
                a = self.read_checked(REG_EXT_ATTR, 4 * n + 2)
                for i, ch in enumerate(chs):
                    ch['precision'], ch['flags'] = a[4 * i], a[4 * i + 1]
            self._channels = chs
        return self._channels

    def poll_length(self) -> int:
        n = self.identity()['count']
        return 2 + 5 * n + (n & 1) + 2

    def poll(self, tries=2) -> dict:
        """計測値を読む。CRC が合わなければ読み直し、だめなら None"""
        chs = self.channels()
        n = len(chs)
        for _ in range(tries):
            b = self.read(REG_POLL, self.poll_length())
            if check_crc(b):
                values = [struct.unpack_from(_TYPE_FMT.get(ch['type'], '<f'), b, 2 + 4 * i)[0]
                          for i, ch in enumerate(chs)]
                return {'seq': struct.unpack_from('<H', b, 0)[0], 'values': values,
                        'status': list(b[2 + 4 * n:2 + 5 * n])}
        return None

    def set_address(self, new_addr: int) -> bool:
        """I2C アドレスを変える。STOP の直後から新アドレスで応答する"""
        if not 0x08 <= new_addr <= 0x77:
            raise ValueError('アドレスは 0x08〜0x77')
        if not self.write(REG_ADDR_KEY, [KEY, new_addr]):
            return False
        self.addr = new_addr
        return True

    # </editor-fold>

    # <editor-fold desc="任意区画">

    def ext_identity(self, refresh=False):
        """拡張識別。未実装なら None"""
        if self._ext is None or refresh:
            b = self.read(REG_EXT_IDENT, 10)
            if b is None:
                raise OslError(f'0x{self.addr:02X}: 拡張識別を読めない')
            if is_unimplemented(b):
                self._ext = False
            elif not check_crc(b):
                raise OslError(f'0x{self.addr:02X}: 拡張識別の CRC 不一致')
            else:
                cap, = struct.unpack_from('<H', b, 0)
                avg, warm = struct.unpack_from('<HH', b, 4)
                self._ext = {'capability': cap, 'hw_rev': b[2],
                             'avg_window_ms': avg, 'warmup_ms': warm}
        return self._ext or None

    def ext_status(self):
        """拡張ステータス。未実装なら None"""
        ext = self.ext_identity()
        if not ext or not ext['capability'] & CAP_EXT_STATUS:
            return None
        b = self.read_checked(REG_EXT_STATUS, 12)
        up, flags, rc, ei, es = struct.unpack_from('<IBBHH', b, 0)
        return {'uptime_s': up, 'node_flags': flags, 'reset_cause': rc,
                'reset_cause_name': RESET_CAUSES.get(rc, str(rc)),
                'err_i2c': ei, 'err_sensor': es}

    def command(self, cmd: int, arg: int = 0) -> bool:
        return self.write(REG_CMD_KEY, [KEY, cmd, arg])

    def reset(self) -> bool:
        return self.command(0x01)

    def identify(self, seconds: int) -> bool:
        return self.command(0x02, seconds)

    # </editor-fold>

    # <editor-fold desc="ユーザー領域 (機種ごと)">

    def user_fields(self):
        """この機種のユーザー領域の項目 [(名前, オフセット, 型, 単位)]。表に無ければ None"""
        return MODELS.get(self.identity()['model'], (None, None))[1]

    def read_user(self) -> dict:
        fields = self.user_fields() or []
        if not fields:
            return {}
        size = max(ofs + struct.calcsize('<' + fmt) for _n, ofs, fmt, _u in fields)
        b = self.read(REG_USER, size)
        if b is None:
            raise OslError(f'0x{self.addr:02X}: ユーザー領域を読めない')
        return {n: struct.unpack_from('<' + fmt, b, ofs)[0] for n, ofs, fmt, _u in fields}

    def write_user(self, name: str, value) -> bool:
        for n, ofs, fmt, _u in self.user_fields() or []:
            if n == name:
                return self.write(REG_USER + ofs, struct.pack('<' + fmt, value))
        raise KeyError(f'この機種のユーザー領域に {name} は無い')

    # </editor-fold>


def scan(bus, first=0x08, last=0x77) -> list:
    """OSL 子機を探す (仕様 §7.1)。[(addr, OslDevice)] を返す。

    番地を送らずに 32 B 読み、"OSL" と CRC を確かめる。この段階では書き込みを
    一切しないので、同じバスにつながった他の I2C 機器を書き換えない。
    """
    found = []
    for addr in range(first, last + 1):
        if not bus.probe(addr):
            continue
        b = bus.i2c_read(addr, IDENT_LEN, retries=1)
        if b is not None and b[0:3] == b'OSL' and check_crc(b):
            found.append((addr, OslDevice(bus, addr)))
    return found


def open_bridge(kind='auto', serial=None, verbose=True):
    """USB-I2C ブリッジを探して開く。kind = 'auto' / 'mcp2221' / 'cp2112'"""
    from mcp2221_driver import MCP2221Device
    from cp2112_driver import CP2112Device
    classes = {'mcp2221': [MCP2221Device], 'cp2112': [CP2112Device]}.get(
        kind, [MCP2221Device, CP2112Device])
    for Bridge in classes:
        for d in Bridge.list_devices():
            if serial is not None and d['serial'] != serial:
                continue
            if verbose:
                name = Bridge.__name__.replace('Device', '')
                print(f'Bridge: {name} - {d["product"]} (S/N: {d["serial"] or "-"})')
            dev = Bridge(slave_addr=0x00, serial_number=d['serial'] or None)
            return dev if dev.open() else None
    return None
