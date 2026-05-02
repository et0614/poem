import time
import struct
from typing import List, Dict, Union, Optional
import sys
from cp2112_driver import CP2112Device

class AnemometerConstants:

    """i2c_shared_data.h に基づく定数定義"""
    # I2C Address
    DEFAULT_I2C_ADDR = 0x10 

    # データ型サイズ定義 (bytes)
    SIZE_UINT8  = 1
    SIZE_FLOAT  = 4
    SIZE_ID     = 16
    SIZE_HASH   = 4
    SIZE_COEF_ARRAY = SIZE_FLOAT * 5  # float[5] = 20 bytes

    # Memory Map Offsets (SensorData_t structure)
    # [Read Only Area] -------------------------
    ADDR_VOLTAGE_HIGH  = 0x00
    ADDR_VOLTAGE_LOW   = ADDR_VOLTAGE_HIGH  + SIZE_UINT8
    ADDR_VOLTAGE_CRC   = ADDR_VOLTAGE_LOW   + SIZE_UINT8

    ADDR_VELOCITY_HIGH = ADDR_VOLTAGE_CRC   + SIZE_UINT8
    ADDR_VELOCITY_LOW  = ADDR_VELOCITY_HIGH + SIZE_UINT8
    ADDR_VELOCITY_CRC  = ADDR_VELOCITY_LOW  + SIZE_UINT8

    ADDR_TEMP_HIGH     = ADDR_VELOCITY_CRC  + SIZE_UINT8
    ADDR_TEMP_LOW      = ADDR_TEMP_HIGH     + SIZE_UINT8
    ADDR_TEMP_CRC      = ADDR_TEMP_LOW      + SIZE_UINT8

    ADDR_STATUS        = ADDR_TEMP_CRC      + SIZE_UINT8
    ADDR_VERSION       = ADDR_STATUS        + SIZE_UINT8

    # 製造ID領域
    ADDR_ID_START      = ADDR_VERSION       + SIZE_UINT8
    ADDR_ID_CRC        = ADDR_ID_START      + SIZE_ID
    ADDR_ID_HASH       = ADDR_ID_CRC        + SIZE_UINT8
    ADDR_ID_HASH_CRC   = ADDR_ID_HASH       + SIZE_HASH

    # [Read/Write Area] ------------------------
    ADDR_ENABLE        = ADDR_ID_HASH_CRC   + SIZE_UINT8
    ADDR_FILTER_N      = ADDR_ENABLE        + SIZE_UINT8
    ADDR_UPDATED       = ADDR_FILTER_N      + SIZE_UINT8

    # 係数領域
    ADDR_COEF_A_START  = ADDR_UPDATED       + SIZE_UINT8
    ADDR_COEF_A_CRC    = ADDR_COEF_A_START  + SIZE_COEF_ARRAY

    ADDR_COEF_B_START  = ADDR_COEF_A_CRC    + SIZE_UINT8
    ADDR_COEF_B_CRC    = ADDR_COEF_B_START  + SIZE_COEF_ARRAY

    # 通信設定
    ADDR_I2C_UNLOCK    = ADDR_COEF_B_CRC    + SIZE_UINT8
    ADDR_I2C_NEW_ADDR  = ADDR_I2C_UNLOCK    + SIZE_UINT8

    # Status Bits
    STATUS_HEATING_MASK = 0x01


class AnemometerManager(CP2112Device):
    def __init__(self, slave_addr=AnemometerConstants.DEFAULT_I2C_ADDR, serial_number=None):
        super().__init__(slave_addr=slave_addr, serial_number=serial_number)
        self._last_comm_time = 0.0  # 最後に通信した時刻（エポック秒）を保持
        self._keep_alive_threshold = 0.950  # マイコンの1000msより少し短めに設定


    def _ensure_wakeup(self):
        """
        最後に通信してから一定時間が経過していれば自動でウェイクアップを実行する
        """
        now = time.time()
        # 経過時間が閾値を超えていれば、マイコンが寝ていると判断して起こす
        if (now - self._last_comm_time) > self._keep_alive_threshold:
            self._robust_wakeup()
        
        # 通信時刻を更新（この後通信が行われるため）
        self._last_comm_time = time.time()


    def _robust_wakeup(self):
        # 念のため前回の残骸を掃除
        self._raw_write([0x17, 0x01])
        time.sleep(0.01)

        # 【ウェイクアップ・パルス】 
        # アドレスのみを書き込む（データ長0）。
        # ここでAVRがクロックストレッチを行い、CP2112がBusy(01)になるはず。
        self._raw_write([0x14, self.slave_addr << 1, 0x00])
        
        # AVRがメインクロックを起動して安定するまで十分に待つ
        # SUT(10us) + VREG(24us) + CPU初期化 を考慮し、余裕を持って2ms待機
        time.sleep(0.002) 

        # 【強制リセット】
        # CP2112が「Busy」で固まっているはずなので、一旦リセットしてバスを解放させる
        self._raw_write([0x17, 0x01])
        time.sleep(0.01)


    def read_i2c_block(self, offset: int, length: int) -> Optional[bytes]:
        """CP2112経由でI2C読み込みする"""
        # 通信の直前にWakeup要否をチェック
        self._ensure_wakeup()

        rslt = super().read_i2c_block(offset, length)
        if rslt is not None:
            # 通信成功後、最終通信時刻を最新に更新
            self._last_comm_time = time.time()
        return rslt


    def write_i2c_block(self, offset: int, data: List[int]) -> bool:
        """CP2112経由でI2C書き込みする"""
        # 通信の直前にWakeup要否をチェック
        self._ensure_wakeup()

        rslt = super().write_i2c_block(offset, data)
        if rslt:
            # 通信成功後、最終通信時刻を最新に更新
            self._last_comm_time = time.time()
        return rslt

    # ------------------------------------------------------------------
    # 公開メソッド: データ取得 (Read Only Area)
    # ------------------------------------------------------------------

    def get_voltage(self) -> Optional[float]:
        """
        現在の電圧値[mV]を取得する
        Returns: 電圧(float), 失敗時 None
        """
        val_bytes = self.read_register_with_crc(AnemometerConstants.ADDR_VOLTAGE_HIGH, 2)
        if val_bytes is None:
            return None
        m_volt = struct.unpack('>H', val_bytes)[0]
        return float(m_volt)


    def get_velocity(self) -> Optional[float]:
        """
        現在の風速[m/s]を取得する
        Note: 基板からは[mm/s]で返ってくるため、1/1000して返す
        """
        val_bytes = self.read_register_with_crc(AnemometerConstants.ADDR_VELOCITY_HIGH, 2)
        if val_bytes is None:
            return None
        # Big Endian uint16
        mm_sec = struct.unpack('>H', val_bytes)[0]
        return mm_sec / 1000.0


    def get_temperature(self) -> Optional[float]:
        """
        現在の温度[C]を取得する
        Note: 基板からは[100*K]で返ってくるため、1/100-273.15して返す
        """
        val_bytes = self.read_register_with_crc(AnemometerConstants.ADDR_TEMP_HIGH, 2)
        if val_bytes is None:
            return None
        # Big Endian signed short
        kelvin100 = struct.unpack('>H', val_bytes)[0]
        return kelvin100 / 100.0 - 273.15
    

    def is_heating(self) -> Dict[str, Union[bool, int]]:
        """
        加熱中か否かを取得する
        """
        raw = self.read_i2c_block(AnemometerConstants.ADDR_STATUS, 1)
        if not raw: return {}

        status_byte = raw[0]
        return bool(status_byte & AnemometerConstants.STATUS_HEATING_MASK)


    def get_version(self) -> int:
        """
        バージョンを取得する
        """
        raw = self.read_i2c_block(AnemometerConstants.ADDR_VERSION, 1)
        return raw[0] if raw else 0
    
    
    def get_unique_id(self) -> Optional[str]:
        """
        16バイトの製造ID+CRCを取得し、16進数文字列として返す
        """
        data_body = self.read_register_with_crc(AnemometerConstants.ADDR_ID_START, 16)
        if data_body is None:
            return None
        # バイト列を2桁の16進数に変換して連結
        return "".join(f"{b:02X}" for b in data_body)
    
    
    def get_id_hash(self) -> Optional[int]:
        """
        製造IDから計算された32bitハッシュ値を取得する
        """
        # 4バイト分読み出す
        data_body = self.read_register_with_crc(AnemometerConstants.ADDR_ID_HASH, 4)
        if data_body is None:
            return None
        # Big Endianとしてアンパック (unsigned int)
        return struct.unpack('>I', data_body)[0]


    # ------------------------------------------------------------------
    # 公開メソッド: 設定変更 (Read/Write Area)
    # ------------------------------------------------------------------

    def set_enable(self, enable: bool) -> bool:
        """
        計測の有効/無効を設定 (1=Enable, 0=Disable/Sleep)
        """
        val = 1 if enable else 0
        return self.write_i2c_block(AnemometerConstants.ADDR_ENABLE, [val])


    def set_filter_n(self, n: int) -> bool:
        """
        平滑化フィルタの係数Nを設定
        """
        n = max(0, min(255, n))
        return self.write_i2c_block(AnemometerConstants.ADDR_FILTER_N, [n])
    

    def get_filter_n(self) -> int:
        """
        平滑化フィルタの係数Nを取得
        """
        return self.read_i2c_block(AnemometerConstants.ADDR_FILTER_N, 0x01)


    def is_updated(self) -> bool:
        """
        データが更新されているか確認する
        """
        # updated レジスタを読み出す
        res = self.read_i2c_block(AnemometerConstants.ADDR_UPDATED, 1)
        return res and res[0] != 0
    
    
    def clear_updated_flag(self):
        self.write_i2c_block(AnemometerConstants.ADDR_UPDATED, [0])


    def set_coefficients_A(self, coefs: List[float]) -> bool:
        """
        係数A (5つのfloat) を設定する
        Note: ファームウェアに合わせてBig-Endian floatとして書き込む
        """
        # 5つのfloatを Big-Endian (>f) でバイト列化 (20 bytes)
        data_bytes = bytearray()
        for f in coefs:
            data_bytes.extend(struct.pack('>f', f))

        return self.write_register_with_crc(AnemometerConstants.ADDR_COEF_A_START, data_bytes)


    def set_coefficients_B(self, coefs: List[float]) -> bool:
        """
        係数B (5つのfloat) を設定する
        Note: ファームウェアに合わせてBig-Endian floatとして書き込む
        """
        # 5つのfloatを Big-Endian (>f) でバイト列化 (20 bytes)
        data_bytes = bytearray()
        for f in coefs:
            data_bytes.extend(struct.pack('>f', f))

        return self.write_register_with_crc(AnemometerConstants.ADDR_COEF_B_START, data_bytes)


    def get_coefficients_A(self) -> Optional[List[float]]:
        """現在の係数Aを読み出す"""
        # float*5 (20bytes) + CRC (1byte)
        data = self.read_register_with_crc(AnemometerConstants.ADDR_COEF_A_START,20)
        
        if data is None:
            return None
        # Big-Endian float としてアンパック
        return list(struct.unpack('>fffff', data))


    def get_coefficients_B(self) -> Optional[List[float]]:
        """現在の係数Bを読み出す"""
        # float*5 (20bytes) + CRC (1byte)
        data = self.read_register_with_crc(AnemometerConstants.ADDR_COEF_B_START,20)
        
        if data is None:
            return None
        # Big-Endian float としてアンパック
        return list(struct.unpack('>fffff', data))


    def change_slave_address(self, new_addr_7bit: int) -> bool:
        """
        I2Cスレーブアドレスを変更する。
        Unlock Key(0xA5) と New Address を連続して書き込むことで変更を確定させる。
        デバイスを再起動するまでアドレス変更は反映されないことに注意
        
        Args:
            new_addr_7bit: 新しい7bit I2Cアドレス (例: 0x20)
        
        Returns:
            bool: 書き込み成功ならTrue
        """
        # マジックナンバー
        UNLOCK_KEY = 0xA5
        
        # [Unlock Key, New Address] の順でデータを構成
        payload = [UNLOCK_KEY, new_addr_7bit]
        
        # ADDR_I2C_UNLOCKから2バイト分一気に書き込む
        return self.write_i2c_block(AnemometerConstants.ADDR_I2C_UNLOCK, payload)


# ------------------------------------------------------------------
# 使用例
# ------------------------------------------------------------------
if __name__ == "__main__":
    # 引数からシリアル番号を取得（なければ None）
    serial_num = sys.argv[1] if len(sys.argv) > 1 else None
    sensor = AnemometerManager(serial_number=serial_num, slave_addr=0x10)

    sensor.open()
    if sensor.is_open:
        print("=== Device Connection: SUCCESS ===")
        
        try:
            # 1. デバイス情報・ステータス取得のテスト
            print("\n--- Testing: Device Information ---")
            uid = sensor.get_unique_id()
            print(f"Unique ID:   {uid}")
            
            hash_val = sensor.get_id_hash()
            print(f"ID Hash:     {hash_val}")
            
            status = sensor.is_heating()
            print(f"Is Heating:  {status}")
            
            version = sensor.get_version()
            print(f"Version:  {version}")

            # 2. 設定変更のテスト
            print("\n--- Testing: Configuration ---")
            print("Setting Enable=True...")
            sensor.set_enable(True)
            
            print("Setting Filter N=6...")
            sensor.set_filter_n(6)

            # 3. センサーデータ取得のテスト
            print("\n--- Testing: Data Acquisition ---")
            # データの更新フラグを確認
            if sensor.is_updated():
                print("Data update flag is SET.")
                sensor.clear_updated_flag()
                print("Data update flag CLEARED.")

            volt = sensor.get_voltage()
            print(f"Voltage:     {volt} mV")
            
            vel = sensor.get_velocity()
            print(f"Velocity:    {vel:.3f} m/s")
            
            vel = sensor.get_temperature()
            print(f"Temperature:    {vel:.2f} C")

            # 4. 係数(Coefficients)の読み書きテスト
            # ※現在の設定値を読み出し、微小に変更して戻すテスト
            print("\n--- Testing: Coefficients (Group A) ---")
            original_coefs = sensor.get_coefficients_A()
            if original_coefs:
                print(f"Original A:  {original_coefs}")
                
                # テスト用に少し値を変更（例：最初の要素に+0.01）
                test_coefs = list(original_coefs)
                test_coefs[0] += 0.01
                
                print("Writing modified coefficients...")
                if sensor.set_coefficients_A(test_coefs):
                    verified_coefs = sensor.get_coefficients_A()
                    print(f"Verified A:  {verified_coefs}")
                    
                    # 元に戻す
                    sensor.set_coefficients_A(original_coefs)
                    print("Restored original coefficients.")
                else:
                    print("Failed to write coefficients (CRC error?).")

            # 5. I2Cアドレス変更（Unlock/Change）のインターフェース確認
            print("\n--- Testing: Method Interfaces ---")
            sensor.change_slave_address(0x10) # 実行する場合は注意
            print("Interface for 'change_slave_address' is ready.")

        except Exception as e:
            print(f"\nAn error occurred during testing: {e}")
        
        finally:
            sensor.close()
            print("\nDevice Closed.")
    else:
        print("Could not open device. Check your connection or 'open()' return value.")