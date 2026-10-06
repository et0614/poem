"""
MCP2221 / MCP2221A (Microchip) USB-I2C ブリッジのドライバ。

cp2112_driver.CP2112Device と同じインタフェース (read_i2c_block / write_i2c_block /
open / close / with 文 / list_devices) にしてあるので、temperature_manager.py などの
基底クラスを差し替えるだけで MCP2221 でも動く。

依存は CP2112 版と同じ hidapi (pip install hidapi) だけ。Windows ではドライバの
インストールは不要 (標準 HID として認識される)。

プロトコル (MCP2221A データシート 3.1 節):
  - 64 byte の HID レポートで 1 コマンド 1 応答。レポート ID は無いので、hidapi の
    write には先頭に 0x00 を付けて 65 byte 渡す。
  - レジスタ読み出しは「書き込み (STOP なし, 0x94)」→「読み出し (Repeated START, 0x93)」
    → 「受信データ取り出し (0x40)」の 3 段。子機 (OSL 共通レジスタ) は CP2112 の
    DATA_WRITE_READ と同じく Repeated START で読まれる前提で作ってある。
  - 1 回に運べるデータは 60 byte。超える分は分割する。
  - NACK やタイムアウトの後は I2C エンジンが固まるので、キャンセル (0x10/0x10) で
    解放しないと次の転送が通らない。
  - 逆に、電源投入後まだ一度も I2C を使っていないときにキャンセルを送ると、エンジンが
    0x62 (STOP タイムアウト) に落ちて二度と戻らない (実機で確認)。速度設定も受け付け
    なくなり、抜け出すにはチップのリセット (0x70) しかない。

内部状態コード (応答 byte 2、ステータス応答 byte 8) はデータシートに載っていない。
値と意味は EasyMCP2221 (MIT, https://github.com/electronicayciencia/EasyMCP2221) が
実機で調べたものに従った。
"""
import time
import hid

# ===== コマンド ===========================================================
CMD_STATUS_SET_PARAMS     = 0x10
CMD_I2C_GET_DATA          = 0x40
CMD_I2C_WRITE             = 0x90
CMD_I2C_READ              = 0x91
CMD_I2C_READ_REPSTART     = 0x93
CMD_I2C_WRITE_NOSTOP      = 0x94
CMD_RESET_CHIP            = 0x70

# CMD_STATUS_SET_PARAMS の byte 2 / byte 3
PARAM_CANCEL_TRANSFER     = 0x10
PARAM_SET_SPEED           = 0x20

# ===== 応答の位置 =========================================================
RESP_RESULT               = 1     # 0x00 = 受け付けた
RESP_I2C_STATE            = 2     # I2C 系コマンドの応答に入る内部状態
RESP_GET_DATA_LEN         = 3     # CMD_I2C_GET_DATA: 有効バイト数
RESP_GET_DATA_BODY        = 4

STATUS_NEWSPEED           = 3     # 0x20 = 速度設定を受け付けた
STATUS_I2C_STATE          = 8
STATUS_I2C_USED           = 21    # 非公開。0 なら電源投入後まだ I2C を使っていない
STATUS_SCL                = 22
STATUS_SDA                = 23
STATUS_FW_MAJOR           = 48
STATUS_FW_MINOR           = 49

# ===== 内部状態 (EasyMCP2221 調べ) =======================================
ST_IDLE                   = 0x00
ST_WRADDRL_NACK_STOP      = 0x25  # アドレスに ACK が返らなかった
ST_WRADDRL_TOUT           = 0x23
ST_WRITEDATA_TOUT         = 0x44
ST_WRITEDATA_END_NOSTOP   = 0x45  # STOP なし書き込みが終わった (Repeated START 待ち)
ST_READDATA_TOUT          = 0x52
ST_READDATA_WAIT          = 0x54  # 受信バッファが満杯、まだ続きがある
ST_READDATA_WAITGET       = 0x55  # 受信完了、取り出し待ち
ST_STOP_TOUT              = 0x62

# 転送中を表す状態。これらの間はポーリングを続ける
ST_BUSY = (0x10, 0x11, 0x15, 0x16,               # START / Repeated START
           0x20, 0x21, 0x22, 0x24,               # アドレス送信
           0x40, 0x41, 0x42, 0x43,               # データ送信
           0x50, 0x51, 0x53,                     # データ受信
           0x60, 0x61)                           # STOP

ST_FAILED = (ST_WRADDRL_NACK_STOP, ST_WRADDRL_TOUT, ST_WRITEDATA_TOUT,
             ST_READDATA_TOUT, ST_STOP_TOUT)

PACKET_SIZE = 64
CHUNK_SIZE  = 60


class MCP2221Device:
    """MCP2221 を使用した I2C 通信の基底クラス (CP2112Device 互換)"""

    VID = 0x04D8    # Vendor ID (Microchip)
    PID = 0x00DD    # Product ID (MCP2221/MCP2221A)

    # 1 転送の完了待ちの上限 [秒]。子機の CLKHOLD 監視 (1 秒) より短くする
    TIMEOUT_SEC = 0.1

    def __init__(self, slave_addr, serial_number=None, speed_hz=100000):
        self.__dev = hid.device()
        self.slave_addr = slave_addr
        self.serial_number = serial_number
        self.speed_hz = speed_hz
        self.is_open = False

    # region with-exitの実装

    def __enter__(self):
        self.open()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()

    # endregion

    # region MCP2221の操作

    def open(self):
        try:
            if self.serial_number is None:
                self.__dev.open(self.VID, self.PID)
            else:
                self.__dev.open(self.VID, self.PID, self.serial_number)
            self.is_open = True
            # 前回の異常終了で I2C エンジンが止まったままのことがあるので、IDLE でなければ
            # 解放する。速度設定は転送中だと拒否されるので、そのあとに行う。
            if self.get_status()['state'] != ST_IDLE:
                self._release()
            if not self.set_speed(self.speed_hz):
                print(f"Warning: I2C speed {self.speed_hz} Hz was not accepted.")
            return True
        except Exception as e:
            print(f"Device open failed: {e}")
            self.is_open = False
            return False

    def close(self):
        if self.is_open:
            self.__dev.close()
            self.is_open = False

    def _cmd(self, buf: list) -> list:
        """64 byte のコマンドを送り、64 byte の応答を返す"""
        packet = [0x00] + list(buf) + [0x00] * (PACKET_SIZE - len(buf))
        self.__dev.write(packet)
        res = self.__dev.read(PACKET_SIZE, 1000)
        if not res or res[0] != buf[0]:
            raise IOError(f"MCP2221: no/invalid response to command 0x{buf[0]:02X}")
        return res

    def get_status(self) -> dict:
        """I2C エンジンの状態と SCL/SDA の実際のレベルを返す"""
        res = self._cmd([CMD_STATUS_SET_PARAMS])
        return {
            'state': res[STATUS_I2C_STATE],
            'scl':   res[STATUS_SCL],
            'sda':   res[STATUS_SDA],
            'used':  res[STATUS_I2C_USED] != 0,
            'fw':    f"{chr(res[STATUS_FW_MAJOR])}.{chr(res[STATUS_FW_MINOR])}",
        }

    def set_speed(self, speed_hz: int) -> bool:
        """I2C クロックを設定 (47 kHz 〜 400 kHz)"""
        divider = round(12_000_000 / speed_hz) - 2
        if not 0 <= divider <= 255:
            raise ValueError("Speed must be between 47 kHz and 400 kHz.")
        res = self._cmd([CMD_STATUS_SET_PARAMS, 0x00, 0x00, PARAM_SET_SPEED, divider])
        return res[STATUS_NEWSPEED] == 0x20

    def _release(self):
        """転送を打ち切って I2C エンジンを IDLE に戻す。
        NACK やタイムアウトの後はこれをしないと次の転送が受け付けられない。
        キャンセルで戻らなければチップをリセットする。"""
        st = self.get_status()
        if st['used']:
            for _ in range(3):
                self._cmd([CMD_STATUS_SET_PARAMS, 0x00, PARAM_CANCEL_TRANSFER])
                st = self.get_status()
                if st['state'] == ST_IDLE and st['scl'] and st['sda']:
                    return True
                time.sleep(0.01)
        if st['state'] == ST_IDLE:
            return True
        # 未使用時のキャンセルで 0x62 に落ちた場合など、キャンセルでは戻らない
        return self.reset_chip()

    def reset_chip(self) -> bool:
        """MCP2221 をリセットして開き直す。USB の再接続が起きるので 2 秒ほどかかる。"""
        try:
            self.__dev.write([0x00, CMD_RESET_CHIP, 0xAB, 0xCD, 0xEF] + [0x00] * 60)
        except OSError:
            pass
        try:
            self.__dev.close()
        except Exception:
            pass
        self.__dev = hid.device()
        for _ in range(30):
            time.sleep(0.1)
            try:
                if self.serial_number:
                    self.__dev.open(self.VID, self.PID, self.serial_number)
                else:
                    self.__dev.open(self.VID, self.PID)
                break
            except OSError:
                continue
        else:
            self.is_open = False
            return False
        self.set_speed(self.speed_hz)
        return self.get_status()['state'] == ST_IDLE

    def _wait_write_done(self) -> bool:
        """書き込み系コマンドの完了を待つ。NACK・タイムアウトで False"""
        deadline = time.perf_counter() + self.TIMEOUT_SEC
        while time.perf_counter() < deadline:
            state = self.get_status()['state']
            if state in (ST_IDLE, ST_WRITEDATA_END_NOSTOP):
                return True
            if state not in ST_BUSY:
                return False
        return False

    def _write(self, cmd: int, addr: int, data: list) -> bool:
        """I2C 書き込み (cmd = CMD_I2C_WRITE / CMD_I2C_WRITE_NOSTOP)"""
        header = [cmd, len(data) & 0xFF, (len(data) >> 8) & 0xFF, (addr << 1) & 0xFF]
        for i in range(0, len(data), CHUNK_SIZE):
            chunk = list(data[i:i + CHUNK_SIZE])
            deadline = time.perf_counter() + self.TIMEOUT_SEC
            while True:
                res = self._cmd(header + chunk)
                if res[RESP_RESULT] == 0x00:
                    break
                # 前の塊をまだ送っている間は受け付けられないので送り直す
                if res[RESP_I2C_STATE] not in ST_BUSY or time.perf_counter() > deadline:
                    return False
        return self._wait_write_done()

    def _read(self, cmd: int, addr: int, length: int):
        """I2C 読み出し (cmd = CMD_I2C_READ / CMD_I2C_READ_REPSTART)。失敗で None"""
        res = self._cmd([cmd, length & 0xFF, (length >> 8) & 0xFF, ((addr << 1) | 1) & 0xFF])
        if res[RESP_RESULT] != 0x00:
            return None

        data = []
        deadline = time.perf_counter() + self.TIMEOUT_SEC
        while time.perf_counter() < deadline:
            res = self._cmd([CMD_I2C_GET_DATA])
            state = res[RESP_I2C_STATE]
            if state in (ST_READDATA_WAIT, ST_READDATA_WAITGET):
                n = res[RESP_GET_DATA_LEN]
                data += res[RESP_GET_DATA_BODY:RESP_GET_DATA_BODY + n]
                if state == ST_READDATA_WAITGET:
                    return bytes(data) if len(data) == length else None
                deadline = time.perf_counter() + self.TIMEOUT_SEC
            elif state in ST_FAILED or state not in ST_BUSY:
                return None
        return None

    # endregion

    # region I2C通信処理 (CP2112Device と同じ)

    def read_i2c_block(self, offset, length, retries=2):
        """レジスタ offset から length バイト読む。
        スレーブ不在 (NACK)・転送エラー・タイムアウト時は None を返す。"""
        for attempt in range(retries + 1):
            try:
                # レジスタ番地を STOP なしで書き、Repeated START で読む
                if self._write(CMD_I2C_WRITE_NOSTOP, self.slave_addr, [offset]):
                    data = self._read(CMD_I2C_READ_REPSTART, self.slave_addr, length)
                    if data is not None:
                        return data
            except IOError:
                pass
            self._release()
            if attempt < retries:
                time.sleep(0.001)
        return None

    def write_i2c_block(self, offset, data: list, retries=2):
        """レジスタ offset から data を書く (1 トランザクション)。成功で True"""
        payload = [offset] + list(data)
        for attempt in range(retries + 1):
            try:
                if self._write(CMD_I2C_WRITE, self.slave_addr, payload):
                    return True
            except IOError:
                pass
            self._release()
            if attempt < retries:
                time.sleep(0.001)
        return False

    def probe(self, addr: int) -> bool:
        """addr に ACK が返るかを調べる (1 byte 読み出し)。I2C スキャン用"""
        try:
            ok = self._read(CMD_I2C_READ, addr, 1) is not None
        except IOError:
            ok = False
        if not ok:
            self._release()
        return ok

    # endregion

    # region その他の処理

    @staticmethod
    def calc_crc8(data: bytes) -> int:
        """CRC8 (Polynomial: 0x31)"""
        crc = 0xFF
        for byte in data:
            crc ^= byte
            for _ in range(8):
                if crc & 0x80:
                    crc = ((crc << 1) ^ 0x31) & 0xFF
                else:
                    crc = (crc << 1) & 0xFF
        return crc

    @classmethod
    def list_devices(cls):
        """接続されているすべての MCP2221 の情報をリストで返す。
        MCP2221 は HID のほかに CDC (仮想 COM ポート) も持つが、enumerate に
        出てくるのは HID インタフェースだけ。"""
        devices = []
        for dev in hid.enumerate(cls.VID, cls.PID):
            path = dev.get('path')
            devices.append({
                'product': dev.get('product_string'),
                'manufacturer': dev.get('manufacturer_string'),
                'serial': dev.get('serial_number'),
                'path': path.decode('utf-8') if isinstance(path, bytes) else path,
            })
        return devices

    # endregion


# --- テスト用メインコード ---
if __name__ == "__main__":
    print("=== MCP2221 Driver Connectivity Test ===")
    devices = MCP2221Device.list_devices()

    if not devices:
        print("No MCP2221 devices found.")
    else:
        print(f"{'No.':<3} | {'Product Name':<20} | {'Serial Number':<15} | {'Path'}")
        print("-" * 80)
        for i, dev in enumerate(devices, 1):
            print(f"{i:<3} | {str(dev['product']):<20} | {str(dev['serial']):<15} | {dev['path']}")

        target = devices[0]
        print(f"Testing device: {target['product']} (S/N: {target['serial']})")

        with MCP2221Device(slave_addr=0x10, serial_number=target['serial']) as device:
            if device.is_open:
                st = device.get_status()
                print(f"Connection established. FW {st['fw']}, "
                      f"SCL={st['scl']} SDA={st['sda']} state=0x{st['state']:02X}")
                if not (st['scl'] and st['sda']):
                    print("SCL/SDA is low: check pull-up resistors / target power.")

                res = device.read_i2c_block(0x00, 1)
                print(f"Read Result: 0x{res.hex()}" if res else "Read Failed (NACK/No Slave)")
            else:
                print("Could not open device.")
