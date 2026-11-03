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
  }
}
