namespace PoEMLink.Link
{
  /// <summary>
  /// SysEx の中で 8bit のデータを送るための 8-to-7 符号化 (通信仕様 §3.2)。
  ///
  /// 元のバイト列を 7 バイトずつに区切り、各組の前に「最上位ビットをまとめたバイト」h を置く
  /// (h の bit i = 組の i 番目のバイトの bit7)。元 n バイトは n + ⌈n/7⌉ バイトになる。
  /// </summary>
  public static class SevenBit
  {
    public static byte[] Encode(ReadOnlySpan<byte> src)
    {
      var dst = new byte[src.Length + (src.Length + 6) / 7];
      int o = 0;
      for (int i = 0; i < src.Length; i += 7)
      {
        int n = Math.Min(7, src.Length - i);
        byte h = 0;
        for (int j = 0; j < n; j++)
          h |= (byte)(((src[i + j] >> 7) & 1) << j);
        dst[o++] = h;
        for (int j = 0; j < n; j++)
          dst[o++] = (byte)(src[i + j] & 0x7F);
      }
      return dst;
    }

    /// <summary>復号する。7bit を超えるバイトや、h だけで終わる組があれば null</summary>
    public static byte[]? Decode(ReadOnlySpan<byte> src)
    {
      var dst = new List<byte>(src.Length);
      int i = 0;
      while (i < src.Length)
      {
        byte h = src[i++];
        int n = Math.Min(7, src.Length - i);
        if (h > 0x7F || n == 0) return null;
        for (int j = 0; j < n; j++)
        {
          byte b = src[i++];
          if (b > 0x7F) return null;
          dst.Add((byte)(b | (((h >> j) & 1) << 7)));
        }
      }
      return dst.ToArray();
    }
  }
}
