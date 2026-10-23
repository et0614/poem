using PoEMLink.Osl;
using PoEMLink.Recording;

namespace PoEMLink.Core.Tests
{
  public class RecorderTests
  {
    [Fact]
    public void Csv_ColumnsGrowAndMissingValuesAreBlank()
    {
      var r = new Recorder();
      var t0 = new DateTime(2026, 10, 23, 9, 0, 0);
      r.Add(t0, new[]
      {
        new RecordCell("A/0", "室内_Temp[°C]", OslValueType.Float32, 24.3, 0),
      });
      // 2 行目で子機 B が増え、A は通信失敗 (値なし)
      r.Add(t0.AddSeconds(1), new[]
      {
        new RecordCell("A/0", "室内_Temp[°C]", OslValueType.Float32, null, 12),
        new RecordCell("B/0", "室内_Temp[°C]", OslValueType.Float32, 101325.5, 0),
        new RecordCell("B/1", "x_Count", OslValueType.UInt32, 42, 0),
      });

      var lines = r.ToCsv().TrimEnd().Split(Environment.NewLine);
      Assert.Equal("Date,Time,室内_Temp[°C],室内_Temp[°C] status,室内_Temp[°C] (2),室内_Temp[°C] (2) status,x_Count,x_Count status", lines[0]);
      Assert.Equal("2026-10-23,09:00:00,24.3,0,,,,", lines[1]);  // float32 の値は丸めずに最短の表記 (24.299999 にしない)
      Assert.Equal("2026-10-23,09:00:01,,12,101325.5,0,42,0", lines[2]);
      Assert.Equal(2, r.Count);
    }

    [Fact]
    public void Csv_QuotesHeadersWithCommas()
    {
      var r = new Recorder();
      r.Add(DateTime.Now, new[] { new RecordCell("A/0", "a,b", OslValueType.Float32, 1, 0) });
      Assert.StartsWith("Date,Time,\"a,b\",\"a,b status\"", r.ToCsv());
    }
  }
}
