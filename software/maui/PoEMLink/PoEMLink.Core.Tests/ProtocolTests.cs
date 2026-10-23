using PoEMLink.Link;
using PoEMLink.Midi;
using PoEMLink.Osl;

namespace PoEMLink.Core.Tests
{
  /// <summary>符号化・CRC・要求の形 (通信仕様 §3.2、§10 の例)</summary>
  public class ProtocolTests
  {
    [Theory]
    [InlineData("102000", "00102000")]
    [InlineData("FF01", "017F01")]
    [InlineData("", "")]
    public void SevenBit_MatchesSpecExamples(string src, string encoded)
    {
      var s = Convert.FromHexString(src);
      Assert.Equal(encoded, Convert.ToHexString(SevenBit.Encode(s)));
      Assert.Equal(src, Convert.ToHexString(SevenBit.Decode(Convert.FromHexString(encoded))!));
    }

    [Fact]
    public void SevenBit_RoundTripsAllLengths()
    {
      var rnd = new Random(1);
      for (int n = 0; n < 40; n++)
      {
        var s = new byte[n];
        rnd.NextBytes(s);
        var e = SevenBit.Encode(s);
        Assert.Equal(n + (n + 6) / 7, e.Length);
        Assert.All(e, b => Assert.True(b < 0x80));
        Assert.Equal(s, SevenBit.Decode(e));
      }
    }

    [Fact]
    public void SevenBit_RejectsInvalid()
    {
      Assert.Null(SevenBit.Decode(new byte[] { 0x00, 0x80 }));  // 7bit を超えるデータ
      Assert.Null(SevenBit.Decode(new byte[] { 0x00 }));        // h だけで終わる組
    }

    [Theory]
    [InlineData(25.0, 4, "25.00")]
    [InlineData(24.387, 4, "24.39")]
    [InlineData(0.1153, 3, "0.115")]
    [InlineData(101325.4, 6, "101325")]
    [InlineData(-3.14159, 3, "-3.14")]
    [InlineData(0.0, 4, "0.000")]
    [InlineData(1.5, 0, "1.50000")]
    public void Channel_FormatKeepsSignificantDigits(double v, byte precision, string expected)
    {
      var ch = new OslChannel(0, "x", 62, OslValueType.Float32, precision, OslChannelFlags.None);
      Assert.Equal(expected, ch.Format(v));
    }

    [Fact]
    public void Crc_CheckValue()
    {
      Assert.Equal(0x29B1, OslCrc.Compute("123456789"u8));
    }

    /// <summary>送った SysEx を記録するだけの通信手段</summary>
    private sealed class CaptureTransport : ILinkTransport
    {
      public List<byte[]> Sent { get; } = new();
#pragma warning disable CS0067
      public event Action<byte[]>? SysExReceived;
      public event Action<bool>? ConnectionChanged;
#pragma warning restore CS0067
      public bool IsConnected => true;
      public string? DeviceName => "capture";
      public void SendSysEx(byte[] body) => Sent.Add(body);
    }

    [Fact]
    public async Task WriteRead_RequestMatchesSpecExample()
    {
      // 仕様 §10: 0x10 の 0x0300 から 14 B → F0 7D 02 06 22 00 10 02 00 03 00 0E 00 F7 (SEQ は 1 から)
      var t = new CaptureTransport();
      using var client = new PoemLinkClient(t) { Timeout = TimeSpan.FromMilliseconds(10), Retries = 1 };
      await Assert.ThrowsAsync<LinkTimeoutException>(() => client.WriteReadAsync(0x10, new byte[] { 0x03, 0x00 }, 14));
      Assert.Equal(2, t.Sent.Count);  // 1 回送り直す (同じ SEQ)
      Assert.Equal("7D020122001002000300" + "0E00", Convert.ToHexString(t.Sent[0]));
      Assert.Equal(t.Sent[0], t.Sent[1]);
    }
  }
}
