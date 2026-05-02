import time
import struct
import sys
from typing import List, Dict, Optional
from cp2112_driver import CP2112Device


class VelocitySensorConstants:
    """
    OSL 共通レジスタ仕様 (SHASE 2026 第2報) + 風速センサの拡張領域
    に基づく定数定義。firmware/poem_velocity_sensor.X/i2c_shared_data.h と同期。

    レイアウト (INFO BLOCK = 走査時 1 回読み, POLL BLOCK = 毎周期読み):
      [INFO BLOCK 0x00-0x27]
      0x00 device_id    (uint32 LE, FNV-1a(SIGROW.SERNUM))
      0x04 addr_key     (uint8  W,  STOP で自動クリア)
      0x05 new_addr     (uint8  R/W)
      0x06 data_count   (uint8  R, =2)
      0x07 reserved
      0x08 unit_type    (uint16[8] LE, BACnet 単位コード)
      0x18 name         (char[16], NUL 終端、EEPROM 永続)

      [POLL BLOCK 0x28-0x4B]
      0x28 status1      (uint8  R, per-value bitmask / 0xFF=ALL_STALE)
      0x29 status2      (uint8  R/W, 1=更新あり、親機が 0 を書き戻して ACK)
      0x2A reserved [2]
      0x2C value        (float[8] LE)

      [拡張領域 0x4C-]
      0x4C enable       (uint8 R/W, 風速計回路起動フラグ)
      0x4D filter_n     (uint8 R/W, 平滑化フィルタ係数 0~20)
      0x4E coefficientA (float[5] LE, 換算係数 A)
      0x62 coefficientB (float[5] LE, 換算係数 B)
    """

    # I2C デフォルトアドレス
    DEFAULT_I2C_ADDR = 0x10

    # 装置ラベル長 (15 文字 + NUL)
    NAME_LEN = 16

    # 共通領域 (INFO BLOCK)
    REG_DEVICE_ID    = 0x00
    REG_ADDR_KEY     = 0x04
    REG_NEW_ADDR     = 0x05
    REG_DATA_COUNT   = 0x06
    REG_UNIT_TYPE    = 0x08
    REG_NAME         = 0x18
    # 共通領域 (POLL BLOCK)
    REG_STATUS1      = 0x28
    REG_STATUS2      = 0x29
    REG_VALUE        = 0x2C

    # 拡張領域
    REG_ENABLE       = 0x4C
    REG_FILTER_N     = 0x4D
    REG_COEF_A       = 0x4E
    REG_COEF_B       = 0x62
    SIZE_COEF_ARRAY  = 4 * 5  # float[5] = 20 bytes

    # 計測値インデックス
    VAL_IDX_VELOCITY = 0
    VAL_IDX_VOLTAGE  = 1

    # アドレス変更鍵
    ADDR_KEY_UNLOCK = 0xA5

    # Status1 (per-value bitmask: bit i = value[i] が stale)
    STATUS1_ALL_STALE = 0xFF

    # BACnet Engineering Units (ASHRAE 135)
    UNIT_VOLTS              = 5
    UNIT_METERS_PER_SECOND  = 74


class VelocitySensorManager(CP2112Device):
    """
    風速センサ子機 (poem_velocity_sensor.X) を CP2112 経由で操作するクラス。

    Note: 風速センサは enable=0 のとき PWR_DOWN スリープに入るため、
          直前通信から閾値秒数以上経過していたら wakeup パルスで起こす必要がある。
    """

    def __init__(self, slave_addr=VelocitySensorConstants.DEFAULT_I2C_ADDR,
                 serial_number=None):
        super().__init__(slave_addr=slave_addr, serial_number=serial_number)
        self._last_comm_time = 0.0
        # 子機側 KEEP_ALIVE_DURATION = 1000ms。少し短めに設定
        self._keep_alive_threshold = 0.950

    # ------------------------------------------------------------------
    # Wakeup (PWR_DOWN からの復帰)
    # ------------------------------------------------------------------

    def _ensure_wakeup(self):
        now = time.time()
        if (now - self._last_comm_time) > self._keep_alive_threshold:
            self._robust_wakeup()
        self._last_comm_time = time.time()

    def _robust_wakeup(self):
        # 前回の残骸を掃除
        self._raw_write([0x17, 0x01])
        time.sleep(0.01)

        # ウェイクアップ・パルス: アドレスのみ書込 (データ長0)。
        # AVR がクロックストレッチしている間に SUT/VREG/CPU 起動を待つ。
        self._raw_write([0x14, self.slave_addr << 1, 0x00])
        time.sleep(0.002)

        # CP2112 を強制リセットしてバスを解放
        self._raw_write([0x17, 0x01])
        time.sleep(0.01)

    def read_i2c_block(self, offset: int, length: int) -> Optional[bytes]:
        self._ensure_wakeup()
        rslt = super().read_i2c_block(offset, length)
        if rslt is not None:
            self._last_comm_time = time.time()
        return rslt

    def write_i2c_block(self, offset: int, data: List[int]) -> bool:
        self._ensure_wakeup()
        rslt = super().write_i2c_block(offset, data)
        if rslt:
            self._last_comm_time = time.time()
        return rslt

    # ------------------------------------------------------------------
    # 共通領域: デバイス情報
    # ------------------------------------------------------------------

    def get_device_id(self) -> Optional[int]:
        """device_id (FNV-1a 32bit ハッシュ, LE) を取得"""
        raw = self.read_i2c_block(VelocitySensorConstants.REG_DEVICE_ID, 4)
        if not raw:
            return None
        return struct.unpack('<I', raw)[0]

    def get_data_count(self) -> Optional[int]:
        """有効計測値の数 (風速センサは 2)"""
        raw = self.read_i2c_block(VelocitySensorConstants.REG_DATA_COUNT, 1)
        return raw[0] if raw else None

    def get_unit_types(self) -> Optional[List[int]]:
        """各計測値の BACnet 単位コード (uint16[8] LE)"""
        raw = self.read_i2c_block(VelocitySensorConstants.REG_UNIT_TYPE, 16)
        if not raw:
            return None
        return list(struct.unpack('<8H', raw))

    def get_new_addr(self) -> Optional[int]:
        """現在の I2C アドレス (EEPROM 値)"""
        raw = self.read_i2c_block(VelocitySensorConstants.REG_NEW_ADDR, 1)
        return raw[0] if raw else None

    # ------------------------------------------------------------------
    # 共通領域: 装置ラベル (EEPROM 永続)
    # ------------------------------------------------------------------

    def get_name(self) -> Optional[str]:
        """装置ラベル (NUL 終端 char[16]) を文字列で取得。空なら ''"""
        raw = self.read_i2c_block(VelocitySensorConstants.REG_NAME,
                                  VelocitySensorConstants.NAME_LEN)
        if raw is None:
            return None
        if len(raw) == 0 or raw[0] == 0xFF:
            return ''
        nul = raw.find(b'\x00')
        return raw[:nul].decode('ascii', errors='replace') if nul >= 0 else \
               raw.decode('ascii', errors='replace')

    def set_name(self, name: str) -> bool:
        """装置ラベルを設定 (NAME_LEN-1 文字以降は切り捨て)。子機側 EEPROM へ永続化される。"""
        encoded = name.encode('ascii', errors='replace')[:VelocitySensorConstants.NAME_LEN - 1]
        payload = list(encoded) + [0] * (VelocitySensorConstants.NAME_LEN - len(encoded))
        return self.write_i2c_block(VelocitySensorConstants.REG_NAME, payload)

    # ------------------------------------------------------------------
    # 共通領域: 状態フラグ
    # ------------------------------------------------------------------

    def get_status1(self) -> Optional[int]:
        """Status1 (0=正常, 0xFF=全計測値が信頼不能 / 予熱中)"""
        raw = self.read_i2c_block(VelocitySensorConstants.REG_STATUS1, 1)
        return raw[0] if raw else None

    def is_all_stale(self) -> bool:
        s1 = self.get_status1()
        return s1 == VelocitySensorConstants.STATUS1_ALL_STALE

    def get_status2(self) -> Optional[int]:
        """Status2 (1=新値あり, 0=未更新/ACK 済)"""
        raw = self.read_i2c_block(VelocitySensorConstants.REG_STATUS2, 1)
        return raw[0] if raw else None

    def clear_status2(self) -> bool:
        """Status2 を 0 に書き戻して新値の取得を ACK"""
        return self.write_i2c_block(VelocitySensorConstants.REG_STATUS2, [0])

    # ------------------------------------------------------------------
    # 共通領域: 計測値
    # ------------------------------------------------------------------

    def get_values(self) -> Optional[List[float]]:
        """8 個の計測値 (float[8], LE) を一括取得"""
        raw = self.read_i2c_block(VelocitySensorConstants.REG_VALUE, 32)
        if not raw:
            return None
        return list(struct.unpack('<8f', raw))

    def get_velocity(self) -> Optional[float]:
        """value[0] = 風速 [m/s]"""
        offset = VelocitySensorConstants.REG_VALUE \
               + VelocitySensorConstants.VAL_IDX_VELOCITY * 4
        raw = self.read_i2c_block(offset, 4)
        if not raw:
            return None
        return struct.unpack('<f', raw)[0]

    def get_voltage(self) -> Optional[float]:
        """value[1] = 風速計回路の生電圧 [V]"""
        offset = VelocitySensorConstants.REG_VALUE \
               + VelocitySensorConstants.VAL_IDX_VOLTAGE * 4
        raw = self.read_i2c_block(offset, 4)
        if not raw:
            return None
        return struct.unpack('<f', raw)[0]

    # ------------------------------------------------------------------
    # 拡張領域: enable / filter_n
    # ------------------------------------------------------------------

    def get_enable(self) -> Optional[int]:
        raw = self.read_i2c_block(VelocitySensorConstants.REG_ENABLE, 1)
        return raw[0] if raw else None

    def set_enable(self, enable: bool) -> bool:
        """風速計回路の起動 (1=Enable, 0=Disable→PWR_DOWN)"""
        return self.write_i2c_block(
            VelocitySensorConstants.REG_ENABLE, [1 if enable else 0])

    def get_filter_n(self) -> Optional[int]:
        raw = self.read_i2c_block(VelocitySensorConstants.REG_FILTER_N, 1)
        return raw[0] if raw else None

    def set_filter_n(self, n: int) -> bool:
        """平滑化フィルタ係数 (0~20)"""
        n = max(0, min(20, n))
        return self.write_i2c_block(
            VelocitySensorConstants.REG_FILTER_N, [n])

    # ------------------------------------------------------------------
    # 拡張領域: 風速換算係数 A / B (float[5] each)
    # ------------------------------------------------------------------

    def get_coefficients_A(self) -> Optional[List[float]]:
        raw = self.read_i2c_block(
            VelocitySensorConstants.REG_COEF_A,
            VelocitySensorConstants.SIZE_COEF_ARRAY)
        if not raw:
            return None
        return list(struct.unpack('<5f', raw))

    def set_coefficients_A(self, coefs: List[float]) -> bool:
        """係数 A (float[5], LE) を一括書き込み"""
        if len(coefs) != 5:
            raise ValueError('coefficients_A must be a list of 5 floats')
        data = struct.pack('<5f', *coefs)
        return self.write_i2c_block(
            VelocitySensorConstants.REG_COEF_A, list(data))

    def get_coefficients_B(self) -> Optional[List[float]]:
        raw = self.read_i2c_block(
            VelocitySensorConstants.REG_COEF_B,
            VelocitySensorConstants.SIZE_COEF_ARRAY)
        if not raw:
            return None
        return list(struct.unpack('<5f', raw))

    def set_coefficients_B(self, coefs: List[float]) -> bool:
        """係数 B (float[5], LE) を一括書き込み"""
        if len(coefs) != 5:
            raise ValueError('coefficients_B must be a list of 5 floats')
        data = struct.pack('<5f', *coefs)
        return self.write_i2c_block(
            VelocitySensorConstants.REG_COEF_B, list(data))

    def get_coefficients(self) -> Optional[Dict[str, List[float]]]:
        """係数 A/B を 1 トランザクションでまとめて取得"""
        raw = self.read_i2c_block(
            VelocitySensorConstants.REG_COEF_A,
            VelocitySensorConstants.SIZE_COEF_ARRAY * 2)
        if not raw:
            return None
        a = list(struct.unpack('<5f', raw[:20]))
        b = list(struct.unpack('<5f', raw[20:40]))
        return {'A': a, 'B': b}

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
        payload = [VelocitySensorConstants.ADDR_KEY_UNLOCK, new_addr_7bit]
        return self.write_i2c_block(
            VelocitySensorConstants.REG_ADDR_KEY, payload)


# ----------------------------------------------------------------------
# 使用例
# ----------------------------------------------------------------------
if __name__ == "__main__":
    serial_num = sys.argv[1] if len(sys.argv) > 1 else None
    sensor = VelocitySensorManager(serial_number=serial_num,
                                   slave_addr=VelocitySensorConstants.DEFAULT_I2C_ADDR)

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
            print(f"Unit[0]:     {units[0]} (期待値: {VelocitySensorConstants.UNIT_METERS_PER_SECOND} = m/s)")
            print(f"Unit[1]:     {units[1]} (期待値: {VelocitySensorConstants.UNIT_VOLTS} = V)")

        name = sensor.get_name()
        print(f"Name:        '{name}'" if name is not None else "Name:        <read failed>")

        # ----- enable / filter -----
        print("\n--- Testing: Configuration ---")
        en = sensor.get_enable()
        fn = sensor.get_filter_n()
        print(f"Enable:      {en}")
        print(f"Filter N:    {fn}")

        if not en:
            print("Setting Enable=1 ...")
            sensor.set_enable(True)

        # ----- 状態 -----
        print("\n--- Testing: Status ---")
        s1 = sensor.get_status1()
        print(f"Status1:     {s1:#04x} ({'ALL STALE / 予熱中' if s1 == 0xFF else 'OK'})")

        s2 = sensor.get_status2()
        print(f"Status2:     {s2}")

        # ----- 計測値 (予熱 5 秒 + α 待つ) -----
        print("\n--- Testing: Measurements (waiting for warm-up) ---")
        for i in range(70):  # 7 秒上限
            time.sleep(0.1)
            if sensor.get_status1() == 0x00 and sensor.get_status2() == 1:
                print(f'  ready after {(i + 1) * 0.1:.1f}s')
                break
        else:
            print('  warm-up did not complete in time, reading anyway')

        vel = sensor.get_velocity()
        volt = sensor.get_voltage()
        print(f"Velocity:    {vel:.3f} m/s" if vel is not None else "Velocity:    <read failed>")
        print(f"Voltage:     {volt:.3f} V" if volt is not None else "Voltage:     <read failed>")

        sensor.clear_status2()
        print("Status2 cleared (ACK).")

        # ----- 係数 A/B 往復テスト -----
        print("\n--- Testing: Coefficients ---")
        coefs = sensor.get_coefficients()
        if coefs:
            print(f"Original A:  {coefs['A']}")
            print(f"Original B:  {coefs['B']}")

            # A の先頭だけ +0.01 して書き戻し → 検証 → 戻す
            test_a = list(coefs['A'])
            test_a[0] += 0.01
            print("Writing test A (A[0] += 0.01)...")
            if sensor.set_coefficients_A(test_a):
                verified = sensor.get_coefficients_A()
                print(f"Verified A:  {verified}")
                sensor.set_coefficients_A(coefs['A'])
                print("Restored original A.")
            else:
                print("Failed to write coefficients_A.")

        # ----- インターフェース確認 (実行注意) -----
        print("\n--- Testing: Method Interfaces ---")
        # sensor.change_slave_address(0x10)  # 実行する場合のみコメント解除
        print("Interface for 'change_slave_address' is ready (not executed).")

        # ----- 連続モニタリング (Ctrl+C で終了) -----
        # 子機は 20ms 周期 (50 Hz) で計測値を更新するので、ここでは 100ms
        # 周期 (10 Hz) でサンプリング表示する
        print("\n--- Live Monitor: voltage / velocity (Ctrl+C to stop) ---")
        print(f"{'time[s]':>8}  {'voltage[V]':>10}  {'velocity[m/s]':>13}  {'st1':>4}")
        t0 = time.time()
        try:
            while True:
                # value[0..1] を 1 トランザクションで取得 (整合性確保)
                raw = sensor.read_i2c_block(
                    VelocitySensorConstants.REG_VALUE, 8)
                if raw is not None and len(raw) == 8:
                    vel, volt = struct.unpack('<2f', raw)
                else:
                    vel, volt = None, None

                s1 = sensor.get_status1()
                sensor.clear_status2()

                v_s = f"{volt:.4f}" if volt is not None else "  --  "
                u_s = f"{vel:.3f}"  if vel  is not None else "  --  "
                s_s = f"0x{s1:02X}" if s1   is not None else "  -- "

                print(f"{time.time() - t0:8.2f}  {v_s:>10}  {u_s:>13}  {s_s:>4}")
                time.sleep(0.1)
        except KeyboardInterrupt:
            print("\nMonitoring stopped by user.")

    except Exception as e:
        print(f"\nAn error occurred during testing: {e}")
    finally:
        sensor.close()
        print("\nDevice Closed.")
