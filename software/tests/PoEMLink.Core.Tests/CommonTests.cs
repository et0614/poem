using PoEM.Common;

namespace PoEMLink.Core.Tests
{
  /// <summary>PoEM.Common (サーバとアプリで共通の表と計算)</summary>
  public class CommonTests
  {
    [Theory]
    [InlineData("0102030405060708090A0B0C0D0E0F10", 951183u)]
    [InlineData("A1A2A3A4A5A6A7A8A9AAABACADAEAFB0", 1843470u)]
    [InlineData("not hex", 0u)]
    public void DeviceId_MatchesV1Fnv1a22(string uid, uint expected)
    {
      // 期待値は v1 のファームウェアの fnv1a_22 と同じ計算を Python で求めたもの
      Assert.Equal(expected, DeviceId.FromUid(uid));
    }

    [Theory]
    [InlineData("0102030405060708090A0B0C0D0E0F10", "1E838F")]  // 951183 = 0x0E838F
    [InlineData("A1A2A3A4A5A6A7A8A9AAABACADAEAFB0", "2C210E")]  // 1843470 = 0x1C210E
    public void DeviceId_DisplayIsHex6(string uid, string expected)
    {
      // 仕様書 5.5 節: 22bit の値に 0x100000 を足して 16 進 6 桁
      Assert.Equal(expected, DeviceId.DisplayFromUid(uid));
    }

    [Theory]
    [InlineData(0u, "100000")]
    [InlineData(0x3FFFFEu, "4FFFFE")]
    public void DeviceId_DisplayRange(uint id, string expected)
    {
      Assert.Equal(expected, DeviceId.ToDisplay(id));
    }

    [Fact]
    public void ModelName()
    {
      Assert.Equal("差圧センサ (SDP810-500Pa)", OslTables.ModelName(0x0303));
      Assert.Equal("機種 0x0999", OslTables.ModelName(0x0999));
      Assert.False(OslTables.TryGetModelName(0x0999, out _));
    }
  }
}
