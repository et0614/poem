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

    [Fact]
    public void ModelName()
    {
      Assert.Equal("差圧センサ (SDP810-500Pa)", OslTables.ModelName(0x0303));
      Assert.Equal("機種 0x0999", OslTables.ModelName(0x0999));
      Assert.False(OslTables.TryGetModelName(0x0999, out _));
    }
  }
}
