using System.Buffers.Binary;

namespace PoEMLink.Osl
{
  /// <summary>OSL v2 の区画の CRC (CRC-16/CCITT-FALSE)</summary>
  public static class OslCrc
  {
    /// <summary>多項式 0x1021、初期値 0xFFFF、反転なし。"123456789" → 0x29B1</summary>
    public static ushort Compute(ReadOnlySpan<byte> data)
    {
      ushort crc = 0xFFFF;
      foreach (var b in data)
      {
        crc ^= (ushort)(b << 8);
        for (int i = 0; i < 8; i++)
          crc = (crc & 0x8000) != 0 ? (ushort)((crc << 1) ^ 0x1021) : (ushort)(crc << 1);
      }
      return crc;
    }

    /// <summary>末尾 2 バイト (LE) が先頭からの CRC と一致するか</summary>
    public static bool Check(ReadOnlySpan<byte> block)
    {
      if (block.Length < 3) return false;
      return Compute(block[..^2]) == BinaryPrimitives.ReadUInt16LittleEndian(block[^2..]);
    }

    /// <summary>任意区画が未実装 (CRC も含めてすべて 0) か</summary>
    public static bool IsUnimplemented(ReadOnlySpan<byte> block) => !block.ContainsAnyExcept((byte)0);
  }
}
