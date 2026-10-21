import hid
import time
from enum import IntEnum

class CP2112Report(IntEnum):
    """CP2112 HID Report IDs"""
    # Configuration
    RESET_DEVICE             = 0x01
    GPIO_CONFIG              = 0x02
    
    # Data Transfer
    DATA_READ_REQ            = 0x10
    DATA_WRITE_READ_REQ      = 0x11
    DATA_READ_FORCE_SEND     = 0x12
    DATA_READ_RESPONSE       = 0x13
    DATA_WRITE_REQ           = 0x14
    
    # Status/Control
    TRANSFER_STATUS_REQ      = 0x15
    TRANSFER_STATUS_RESPONSE = 0x16
    CANCEL_TRANSFER          = 0x17


class I2CStatus(IntEnum):
    """Report ID 0x16 (TRANSFER_STATUS_RESPONSE) の res[1] ステータスコード"""
    IDLE           = 0x00
    BUSY           = 0x01
    COMPLETE       = 0x02
    ERROR          = 0x03
    

class CP2112Device:
    """CP2112チップを使用したI2C通信の基底クラス"""
    
    # region 定数宣言,初期化処理

    VID = 0x10C4    # Vendor ID
    PID = 0xEA90    # Product ID

    def __init__(self, slave_addr, serial_number):
        self.__dev = hid.device()
        self.slave_addr = slave_addr
        self.serial_number = serial_number
        self.is_open = False

    #endregion

    # region with-exitの実装

    def __enter__(self):
        self.open()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()

    # endregion

    # region CP2112の操作

    def open(self):
        try:
            if self.serial_number is None:
                self.__dev.open(self.VID, self.PID)
            else:
                self.__dev.open(self.VID, self.PID, self.serial_number)
            self.__dev.write([
                CP2112Report.CANCEL_TRANSFER,
                0x01]) # Transfer Reset
            self._configure_smbus()
            self.is_open = True
            return True
        except Exception as e:
            print(f"Device open failed: {e}")
            self.is_open = False
            return False # 失敗を返す
        

    def close(self):
        if self.is_open:
            self.__dev.close()
            self.is_open = False


    # SMBus の設定 (Feature Report 0x06、AN495)
    SMBUS_CONFIG     = 0x06
    SPEED_HZ         = 100000
    WRITE_TIMEOUT_MS = 100
    READ_TIMEOUT_MS  = 100
    RETRY_LIMIT      = 1

    def _configure_smbus(self):
        """NACK の再送回数とタイムアウトを設定する。

        CP2112 の既定は「NACK を受けたら打ち切るまで無制限に再送」「タイムアウトなし」。
        存在しないアドレスを調べるたびに打ち切りまで START を送り続けるので、走査に
        1 アドレス約 160 ms (全体で約 18 秒) かかっていた。再送を 1 回に絞り、読み書きの
        タイムアウトと SCL の Low 張り付き検出 (25 ms) を有効にする。
        設定は RAM にだけ書く (CP2112 の電源を切ると既定に戻る)。"""
        cfg = [self.SMBUS_CONFIG]
        cfg += list(self.SPEED_HZ.to_bytes(4, 'big'))
        cfg += [0x02,                                   # CP2112 自身のアドレス (既定値)
                0x00]                                   # Auto Send Read: 無効
        cfg += list(self.WRITE_TIMEOUT_MS.to_bytes(2, 'big'))
        cfg += list(self.READ_TIMEOUT_MS.to_bytes(2, 'big'))
        cfg += [0x01]                                   # SCL Low タイムアウト: 有効
        cfg += list(self.RETRY_LIMIT.to_bytes(2, 'big'))
        self.__dev.send_feature_report(cfg)

    def _raw_write(self, data: list):
        """CP2112に書き込む"""
        return self.__dev.write(data)


    def _raw_read(self, length: int, timeout: int):
        """CP2112を読み込む"""
        return self.__dev.read(length, timeout)


    # endregion

    # region I2C通信処理

    def read_i2c_block(self, offset, length, retries=2):
        """低レイヤーI2C読み込み (CP2112コマンド処理)。

        スレーブ不在(NACK)・転送エラー・タイムアウト時は None を返す。
        転送の COMPLETE と DATA_READ_RESPONSE の有効バイト数を検証するので、
        HID バッファの残骸(ゴミ)を有効データとして返すことはない。
        """
        for attempt in range(retries + 1):
            try:
                self.__dev.write([
                    CP2112Report.DATA_WRITE_READ_REQ,
                    self.slave_addr << 1,
                    (length >> 8) & 0xFF,
                    length & 0xFF,
                    0x01, # Target Address Length (通常1)
                    offset])

                # 転送完了までポーリング。COMPLETE を実際に確認できたかを記録する。
                # NACK(スレーブ不在)や ERROR のときは COMPLETE にならないので失敗扱い。
                completed = False
                for _ in range(15):
                    self.__dev.write([
                        CP2112Report.TRANSFER_STATUS_REQ,
                        0x01])
                    res = self.__dev.read(64, 100)
                    if res and res[0] == CP2112Report.TRANSFER_STATUS_RESPONSE:
                        status = res[1]
                        if status == I2CStatus.COMPLETE:
                            completed = True
                            break
                        if status == I2CStatus.ERROR:
                            # NACK / バスエラー。これ以上待っても無駄なので抜ける。
                            break
                    time.sleep(0.001)

                if not completed:
                    # 完了が確認できない(不在/NACK/タイムアウト)。中途半端な転送を
                    # 破棄してからリトライ。ゴミは返さない。
                    self.__dev.write([CP2112Report.CANCEL_TRANSFER, 0x01])
                    if attempt < retries:
                        time.sleep(0.001)
                    continue

                # データ取得
                self.__dev.write([
                    CP2112Report.DATA_READ_FORCE_SEND,
                    0x00,
                    length])
                data_res = self.__dev.read(64, 500)
                # DATA_READ_RESPONSE = [0x13, status, valid_len, data...]
                # valid_len(data_res[2]) が要求長に満たなければ失敗扱い(ゴミを返さない)。
                if (data_res and
                        data_res[0] == CP2112Report.DATA_READ_RESPONSE and
                        len(data_res) >= 3 and
                        data_res[2] >= length):
                    return bytes(data_res[3:3+length]) # bytes型で返す

            except Exception as e:
                if attempt == retries: return None
                time.sleep(0.001)
        return None


    def write_i2c_block(self, offset, data: list, retries=2):
        """低レイヤーI2C書き込み"""
        write_data = [
            CP2112Report.DATA_WRITE_REQ, 
            self.slave_addr << 1, 
            len(data) + 1,   # DataLen (Offset + Data)
            offset,          # 書き込み開始位置
        ] + list(data)

        for attempt in range(retries + 1):
            try:
                self.__dev.write(write_data)
                # ステータスポーリング
                for _ in range(15):
                    self.__dev.write([
                        CP2112Report.TRANSFER_STATUS_REQ,
                        0x01])
                    res = self.__dev.read(64, 100)
                    if (res and 
                        res[0] == CP2112Report.TRANSFER_STATUS_RESPONSE and 
                        res[1] == I2CStatus.COMPLETE):
                        return True
                    time.sleep(0.001)
            except Exception as e:
                if attempt == retries:
                    print(f"I2C Write Fatal Error: {e}")
                    return False
            
            if attempt < retries:
                time.sleep(0.001) # リトライ前の待機
        return False


    def read_register_with_crc(self, offset, data_len, retries=2):
        """
        [データnバイト + CRC1バイト] 形式の読み込みを行う共通関数
        """
        for attempt in range(retries + 1):
            raw = self.read_i2c_block(offset, data_len + 1)
            
            if raw:
                data_body = raw[:-1]
                chk_crc = raw[-1]
                if self.calc_crc8(data_body) == chk_crc:
                    return data_body
            
            if attempt < retries:
                time.sleep(0.001)

        return None
    

    def write_register_with_crc(self, offset, data: bytes, retries=2):
        """
        [データnバイト + CRC1バイト] を計算して書き込む共通関数
        """
        crc = self.calc_crc8(data)
        # データ(bytes)をリスト化して末尾にCRCを追加
        payload = list(data) + [crc]

        for attempt in range(retries + 1):
            if self.write_i2c_block(offset, payload):
                return True
            
            if attempt < retries:
                time.sleep(0.001)

        return False

    # endregion

    # region 汎用 I2C (OSL v2 用。番地の形を問わない)

    # 1 回の書き込みの上限 [byte]。CP2112 の Data Write Request の上限で、これを超える
    # 書き込みは 1 つの I2C 転送にできない。読み出しは 512 byte まで。
    MAX_WRITE = 61
    MAX_READ  = 512

    def _wait_complete(self) -> bool:
        """転送の完了を待つ。NACK・バス異常・タイムアウトで False (中途の転送は取り消す)"""
        for _ in range(50):
            self.__dev.write([CP2112Report.TRANSFER_STATUS_REQ, 0x01])
            res = self.__dev.read(64, 100)
            if res and res[0] == CP2112Report.TRANSFER_STATUS_RESPONSE:
                if res[1] == I2CStatus.COMPLETE:
                    return True
                if res[1] == I2CStatus.ERROR:
                    break
            time.sleep(0.001)
        self.__dev.write([CP2112Report.CANCEL_TRANSFER, 0x01])
        return False

    def _drain(self):
        """受信待ちに残っている古い応答を捨てる。

        応答の数が要求と食い違うと、残った応答を次の転送が自分の応答と取り違え、
        以降の転送がすべて失敗する。転送の前に必ず空にしておく。"""
        # read(…, 0) は「待たない」ではなく「届くまで待つ」になる (hidapi の仕様) ので 1 ms にする
        for _ in range(64):
            if not self.__dev.read(64, 1):
                break

    def _fetch(self, length: int):
        """受信済みのデータを取り出す。

        送信要求 (Force Send) は全体の長さで 1 回だけ出す。CP2112 は 1 回の応答に
        最大 61 byte しか入れられないので、61 byte を超えるときは応答が複数届く。
        残りの長さで送信要求を出し直すと応答が余分に 1 つ返り、受信待ちに残ってしまう。"""
        self.__dev.write([CP2112Report.DATA_READ_FORCE_SEND,
                          (length >> 8) & 0xFF, length & 0xFF])
        data = []
        for _ in range(length // 61 + 4):
            if len(data) >= length:
                break
            res = self.__dev.read(64, 500)
            if not res or res[0] != CP2112Report.DATA_READ_RESPONSE or len(res) < 3:
                return None
            data += res[3:3 + res[2]]
        return bytes(data[:length]) if len(data) >= length else None

    def i2c_write(self, addr: int, data, retries=2) -> bool:
        """S, adr+W, data…, P。成功で True。61 byte を超える書き込みはできない"""
        data = list(data)
        if not data or len(data) > self.MAX_WRITE:
            raise ValueError(f'CP2112 の書き込みは 1〜{self.MAX_WRITE} byte')
        for attempt in range(retries + 1):
            try:
                self._drain()
                self.__dev.write([CP2112Report.DATA_WRITE_REQ, addr << 1, len(data)] + data)
                if self._wait_complete():
                    return True
            except Exception:
                pass
            if attempt < retries:
                time.sleep(0.001)
        return False

    def i2c_read(self, addr: int, length: int, retries=2):
        """S, adr+R, data…, P (番地を送らない読み出し)。失敗で None"""
        for attempt in range(retries + 1):
            try:
                self._drain()
                self.__dev.write([CP2112Report.DATA_READ_REQ, addr << 1,
                                  (length >> 8) & 0xFF, length & 0xFF])
                if self._wait_complete():
                    data = self._fetch(length)
                    if data is not None:
                        return data
            except Exception:
                pass
            if attempt < retries:
                time.sleep(0.001)
        return None

    def i2c_write_read(self, addr: int, wdata, length: int, retries=2):
        """S, adr+W, wdata…, Sr, adr+R, data…, P。wdata は 1〜16 byte。失敗で None"""
        wdata = list(wdata)
        if not 1 <= len(wdata) <= 16:
            raise ValueError('CP2112 の番地は 1〜16 byte')
        for attempt in range(retries + 1):
            try:
                self._drain()
                self.__dev.write([CP2112Report.DATA_WRITE_READ_REQ, addr << 1,
                                  (length >> 8) & 0xFF, length & 0xFF, len(wdata)] + wdata)
                if self._wait_complete():
                    data = self._fetch(length)
                    if data is not None:
                        return data
            except Exception:
                pass
            if attempt < retries:
                time.sleep(0.001)
        return None

    def probe(self, addr: int) -> bool:
        """addr に ACK が返るかを調べる (1 byte 読み出し、書き込みはしない)"""
        return self.i2c_read(addr, 1, retries=0) is not None

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
        """
        接続されているすべてのCP2112デバイスの情報を取得し、リストで返す。
        """
        # デバイスの列挙
        raw_devices = hid.enumerate(CP2112Device.VID, CP2112Device.PID)
        
        devices = []
        for dev in raw_devices:
            # パスのデコード処理
            path = dev.get('path')
            info = {
                'product': dev.get('product_string'),
                'manufacturer': dev.get('manufacturer_string'),
                'serial': dev.get('serial_number'),
                'path': path.decode('utf-8') if isinstance(path, bytes) else path
            }
            devices.append(info)
        
        return devices
    
    # endregion


    # --- テスト用メインコード ---
if __name__ == "__main__":    
    print("=== CP2112 Driver Connectivity Test ===")
    devices = CP2112Device.list_devices()
    
    if not devices:
        print("No CP2112 devices found.")
    else:
        # 検出されたデバイスを表示
        print(f"{'No.':<3} | {'Product Name':<20} | {'Serial Number':<15} | {'Path'}")
        print("-" * 80)
        for i, dev in enumerate(devices, 1):
            print(f"{i:<3} | {dev['product']:<20} | {dev['serial']:<15} | {dev['path']}")

        # 最初のデバイスで接続テスト
        target = devices[0]
        print(f"Testing device: {target['product']} (S/N: {target['serial']})")
        
        with CP2112Device(slave_addr=0x10, serial_number=target['serial']) as device:
            if device.is_open:
                print("Connection established successfully.")
                # CRCテスト
                test_bytes = b'\x12\x34\x56'
                print(f"CRC8 test for {test_bytes.hex()}: 0x{device.calc_crc8(test_bytes):02X}")
                
                # I2C疎通確認 (レジスタ0x00を1バイト読み込み)
                res = device.read_i2c_block(0x00, 1)
                status_str = f"Read Result: 0x{res.hex()}" if res else "Read Failed (NACK/No Slave)"
                print(status_str)
            else:
                print("Failed to open device.")