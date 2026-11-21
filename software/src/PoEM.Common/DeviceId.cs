namespace PoEM.Common
{
  /// <summary>
  /// 子機の Device ID (22bit)。16 B の UID (OSL v2 の device_id) から求める。
  ///
  /// v1 の子機ファームウェアの fnv1a_22 と同じ計算: FNV-1a (32bit) を XOR で 22bit に畳み、
  /// BACnet の予約値 0x3FFFFF を避ける。v2 の UID は v1 がハッシュしていたマイコン固有番号
  /// (SIGROW.SERNUM) そのものなので、同じ子機なら v1 の device_id と同じ値になる。
  /// </summary>
  public static class DeviceId
  {
    public static uint FromUid(ReadOnlySpan<byte> uid)
    {
      uint h = 2166136261u;
      foreach (byte x in uid)
      {
        h ^= x;
        h *= 16777619u;
      }
      h = (h ^ (h >> 22)) & 0x3FFFFFu;
      return h == 0x3FFFFFu ? 0x3FFFFEu : h;
    }

    /// <summary>UID を 16 進 32 文字で受け取る。16 進として読めなければ 0</summary>
    public static uint FromUid(string uidHex)
    {
      try
      {
        return FromUid(Convert.FromHexString(uidHex));
      }
      catch (FormatException)
      {
        return 0;
      }
    }

    /// <summary>
    /// 表示用 ID の文字列 (仕様書 5.5 節)。22bit の値に 0x100000 を足し、16 進 6 桁 (大文字) で表す
    /// (100000〜4FFFFE)。桁数を常に 6 桁にそろえ、先頭が 0 にならないようにする。
    /// </summary>
    public static string ToDisplay(uint id) => (id + 0x100000u).ToString("X6");

    /// <summary>UID (16 進 32 文字) から表示用 ID の文字列を求める</summary>
    public static string DisplayFromUid(string uidHex) => ToDisplay(FromUid(uidHex));
  }
}
