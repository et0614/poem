import time
import struct
import sys
from typing import List, Dict, Optional
from cp2112_driver import CP2112Device


class TemperatureSensorConstants:
    """
    OSL 共通レジスタ仕様 (SHASE 2026 第2報) + 温湿度センサの拡張領域
    に基づく定数定義。firmware/poem_temperature_sensor.X/i2c_shared_data.h と同期。

    レイアウト (INFO BLOCK = 走査時 1 回読み, POLL BLOCK = 毎周期読み):
      [INFO BLOCK 0x00-0x27]
      0x00 device_id  (uint32 LE, FNV-1a(SIGROW.SERNUM))
      0x04 addr_key   (uint8  W,  STOP で自動クリア)
      0x05 new_addr   (uint8  R/W)
      0x06 data_count (uint8  R, =2)
      0x07 reserved
      0x08 unit_type  (uint16[8] LE, BACnet 単位コード)
      0x18 name       (char[16], NUL 終端、EEPROM 永続)

      [POLL BLOCK 0x28-0x4B]
      0x28 status1    (uint8  R, per-value bitmask / 0xFF=ALL_STALE)
      0x29 status2    (uint8  R/W, 1=更新あり、親機が 0 を書き戻して ACK)
      0x2A reserved [2]
      0x2C value      (float[8] LE)

      [拡張領域 0x4C-]
      0x4C temp_coef_a / 0x50 temp_coef_b
      0x54 humi_coef_a / 0x58 humi_coef_b
    """

    # I2C デフォルトアドレス
    DEFAULT_I2C_ADDR = 0x10

    # 装置ラベル長 (15 文字 + NUL)
    NAME_LEN = 16

    # 共通領域 (INFO BLOCK)
    REG_DEVICE_ID   = 0x00
    REG_ADDR_KEY    = 0x04
    REG_NEW_ADDR    = 0x05
    REG_DATA_COUNT  = 0x06
    REG_UNIT_TYPE   = 0x08
    REG_NAME        = 0x18
    # 共通領域 (POLL BLOCK)
    REG_STATUS1     = 0x28
    REG_STATUS2     = 0x29
    REG_VALUE       = 0x2C

    # 拡張領域 (線形補正 corrected = a * raw + b)
    REG_TEMP_COEF_A = 0x4C
    REG_TEMP_COEF_B = 0x50
    REG_HUMI_COEF_A = 0x54
    REG_HUMI_COEF_B = 0x58

    # 計測値インデックス
    VAL_IDX_TEMPERATURE = 0
    VAL_IDX_HUMIDITY    = 1

    # アドレス変更鍵
    ADDR_KEY_UNLOCK = 0xA5

    # Status1 (per-value bitmask: bit i = value[i] が stale)
    STATUS1_ALL_STALE = 0xFF

    # BACnet Engineering Units (ASHRAE 135)
    UNIT_DEGREES_CELSIUS        = 62
    UNIT_PERCENT_RELATIVE_HUMID = 29


class TemperatureSensorManager(CP2112Device):
    """
    温湿度センサ子機 (poem_temperature_sensor.X) を CP2112 経由で操作するクラス。

    Note: 温湿度センサのファームは IDLE スリープ固定なので、anemometer のような
          PWR_DOWN 復帰用の wakeup パルスは不要。
    """

    def __init__(self, slave_addr=TemperatureSensorConstants.DEFAULT_I2C_ADDR,
                 serial_number=None):
        super().__init__(slave_addr=slave_addr, serial_number=serial_number)

    # ------------------------------------------------------------------
    # 共通領域: デバイス情報
    # ------------------------------------------------------------------

    def get_device_id(self) -> Optional[int]:
        """device_id (FNV-1a 32bit ハッシュ, LE) を取得"""
        raw = self.read_i2c_block(TemperatureSensorConstants.REG_DEVICE_ID, 4)
        if not raw:
            return None
        return struct.unpack('<I', raw)[0]

    def get_data_count(self) -> Optional[int]:
        """有効計測値の数 (温湿度センサは 2)"""
        raw = self.read_i2c_block(TemperatureSensorConstants.REG_DATA_COUNT, 1)
        return raw[0] if raw else None

    def get_unit_types(self) -> Optional[List[int]]:
        """各計測値の BACnet 単位コード (uint16[8] LE)"""
        raw = self.read_i2c_block(TemperatureSensorConstants.REG_UNIT_TYPE, 16)
        if not raw:
            return None
        return list(struct.unpack('<8H', raw))

    def get_new_addr(self) -> Optional[int]:
        """現在の I2C アドレス (EEPROM 値)"""
        raw = self.read_i2c_block(TemperatureSensorConstants.REG_NEW_ADDR, 1)
        return raw[0] if raw else None

    # ------------------------------------------------------------------
    # 共通領域: 装置ラベル (EEPROM 永続)
    # ------------------------------------------------------------------

    def get_name(self) -> Optional[str]:
        """装置ラベル (NUL 終端 char[16]) を文字列で取得。空なら ''"""
        raw = self.read_i2c_block(TemperatureSensorConstants.REG_NAME,
                                  TemperatureSensorConstants.NAME_LEN)
        if raw is None:
            return None
        # 最初の NUL までを切り出し (0xFF 始まりの未初期化領域も空扱い)
        if len(raw) == 0 or raw[0] == 0xFF:
            return ''
        nul = raw.find(b'\x00')
        return raw[:nul].decode('ascii', errors='replace') if nul >= 0 else \
               raw.decode('ascii', errors='replace')

    def set_name(self, name: str) -> bool:
        """装置ラベルを設定 (NAME_LEN-1 文字以降は切り捨て)。子機側 EEPROM へ永続化される。"""
        encoded = name.encode('ascii', errors='replace')[:TemperatureSensorConstants.NAME_LEN - 1]
        # NAME_LEN バイトに NUL パディング
        payload = list(encoded) + [0] * (TemperatureSensorConstants.NAME_LEN - len(encoded))
        return self.write_i2c_block(TemperatureSensorConstants.REG_NAME, payload)

    # ------------------------------------------------------------------
    # 共通領域: 状態フラグ
    # ------------------------------------------------------------------

    def get_status1(self) -> Optional[int]:
        """Status1 (0=正常, 0xFF=全計測値が信頼不能)"""
        raw = self.read_i2c_block(TemperatureSensorConstants.REG_STATUS1, 1)
        return raw[0] if raw else None

    def is_all_stale(self) -> bool:
        s1 = self.get_status1()
        return s1 == TemperatureSensorConstants.STATUS1_ALL_STALE

    def get_status2(self) -> Optional[int]:
        """Status2 (1=新値あり, 0=未更新/ACK 済)"""
        raw = self.read_i2c_block(TemperatureSensorConstants.REG_STATUS2, 1)
        return raw[0] if raw else None

    def clear_status2(self) -> bool:
        """Status2 を 0 に書き戻して新値の取得を ACK"""
        return self.write_i2c_block(TemperatureSensorConstants.REG_STATUS2, [0])

    # ------------------------------------------------------------------
    # 共通領域: 計測値
    # ------------------------------------------------------------------

    def get_values(self) -> Optional[List[float]]:
        """8 個の計測値 (float[8], LE) を一括取得"""
        raw = self.read_i2c_block(TemperatureSensorConstants.REG_VALUE, 32)
        if not raw:
            return None
        return list(struct.unpack('<8f', raw))

    def get_temperature(self) -> Optional[float]:
        """value[0] = 温度 [°C]"""
        offset = TemperatureSensorConstants.REG_VALUE \
               + TemperatureSensorConstants.VAL_IDX_TEMPERATURE * 4
        raw = self.read_i2c_block(offset, 4)
        if not raw:
            return None
        return struct.unpack('<f', raw)[0]

    def get_humidity(self) -> Optional[float]:
        """value[1] = 相対湿度 [%RH]"""
        offset = TemperatureSensorConstants.REG_VALUE \
               + TemperatureSensorConstants.VAL_IDX_HUMIDITY * 4
        raw = self.read_i2c_block(offset, 4)
        if not raw:
            return None
        return struct.unpack('<f', raw)[0]

    # ------------------------------------------------------------------
    # 拡張領域: 線形補正係数 (corrected = a * raw + b)
    # ------------------------------------------------------------------

    def get_calibration(self) -> Optional[Dict[str, float]]:
        """温湿度の補正係数 (a, b) を一括取得"""
        raw = self.read_i2c_block(TemperatureSensorConstants.REG_TEMP_COEF_A, 16)
        if not raw:
            return None
        ta, tb, ha, hb = struct.unpack('<4f', raw)
        return {'temp_a': ta, 'temp_b': tb, 'humi_a': ha, 'humi_b': hb}

    def set_calibration(self, temp_a: float, temp_b: float,
                        humi_a: float, humi_b: float) -> bool:
        """温湿度の補正係数 (a, b) を 1 トランザクションで書き込む。
           子機は拡張領域への書き込み完了 (STOP) で EEPROM へコミットする。"""
        data = struct.pack('<4f', temp_a, temp_b, humi_a, humi_b)
        return self.write_i2c_block(
            TemperatureSensorConstants.REG_TEMP_COEF_A, list(data))

    def reset_calibration(self) -> bool:
        """補正を無効化 (a=1.0, b=0.0)"""
        return self.set_calibration(1.0, 0.0, 1.0, 0.0)

    # ------------------------------------------------------------------
    # アドレス変更
    # ------------------------------------------------------------------

    def change_slave_address(self, new_addr_7bit: int) -> bool:
        """
        I2C スレーブアドレスを変更する。
        addr_key (0xA5) と new_addr を 1 トランザクションで連続書き込みし、
        STOP のタイミングで EEPROM へコミットされる。

        Note: 反映は次回起動時。
        """
        payload = [TemperatureSensorConstants.ADDR_KEY_UNLOCK, new_addr_7bit]
        return self.write_i2c_block(
            TemperatureSensorConstants.REG_ADDR_KEY, payload)


# ----------------------------------------------------------------------
# 使用例
# ----------------------------------------------------------------------
if __name__ == "__main__":
    serial_num = sys.argv[1] if len(sys.argv) > 1 else None
    sensor = TemperatureSensorManager(serial_number=serial_num,
                                      slave_addr=TemperatureSensorConstants.DEFAULT_I2C_ADDR)

    sensor.open()
    if not sensor.is_open:
        print("Could not open device. Check your connection.")
        sys.exit(1)

    print("=== Device Connection: SUCCESS ===")
    try:
        # ----- デバイス情報 -----
        print("\n--- Testing: Device Information ---")
        dev_id = sensor.get_device_id()
        print(f"Device ID:   {dev_id:#010x}" if dev_id is not None else "Device ID:   <read failed>")

        cnt = sensor.get_data_count()
        print(f"Data Count:  {cnt}")

        addr = sensor.get_new_addr()
        print(f"I2C Addr:    {addr:#04x}" if addr is not None else "I2C Addr:    <read failed>")

        units = sensor.get_unit_types()
        if units:
            print(f"Unit[0]:     {units[0]} (期待値: {TemperatureSensorConstants.UNIT_DEGREES_CELSIUS} = degC)")
            print(f"Unit[1]:     {units[1]} (期待値: {TemperatureSensorConstants.UNIT_PERCENT_RELATIVE_HUMID} = %RH)")

        name = sensor.get_name()
        print(f"Name:        '{name}'" if name is not None else "Name:        <read failed>")

        # ----- 状態 -----
        print("\n--- Testing: Status ---")
        s1 = sensor.get_status1()
        print(f"Status1:     {s1:#04x} ({'ALL STALE' if s1 == 0xFF else 'OK'})")

        s2 = sensor.get_status2()
        print(f"Status2:     {s2}")

        # ----- 計測値 -----
        print("\n--- Testing: Measurements ---")
        # 新値が来るのを少し待つ (RTC 1sec 周期)
        for _ in range(20):
            if sensor.get_status2() == 1:
                break
            time.sleep(0.1)

        temp = sensor.get_temperature()
        humi = sensor.get_humidity()
        print(f"Temperature: {temp:.2f} C" if temp is not None else "Temperature: <read failed>")
        print(f"Humidity:    {humi:.2f} %RH" if humi is not None else "Humidity:    <read failed>")

        # 取得を ACK
        sensor.clear_status2()
        print("Status2 cleared (ACK).")

        # ----- 補正係数 -----
        print("\n--- Testing: Calibration ---")
        cal = sensor.get_calibration()
        if cal:
            print(f"Original:    temp y = {cal['temp_a']} * x + {cal['temp_b']}")
            print(f"             humi y = {cal['humi_a']} * x + {cal['humi_b']}")

            # 微小オフセットで往復テスト
            print("Writing test calibration (temp_b += 0.5)...")
            sensor.set_calibration(
                cal['temp_a'], cal['temp_b'] + 0.5,
                cal['humi_a'], cal['humi_b'])

            verified = sensor.get_calibration()
            print(f"Verified:    temp y = {verified['temp_a']} * x + {verified['temp_b']}")

            # 元に戻す
            sensor.set_calibration(cal['temp_a'], cal['temp_b'],
                                   cal['humi_a'], cal['humi_b'])
            print("Restored original calibration.")

        # ----- インターフェース確認 (実行注意) -----
        print("\n--- Testing: Method Interfaces ---")
        # sensor.change_slave_address(0x10)  # 実行する場合のみコメント解除
        print("Interface for 'change_slave_address' is ready (not executed).")

    except Exception as e:
        print(f"\nAn error occurred during testing: {e}")
    finally:
        sensor.close()
        print("\nDevice Closed.")
