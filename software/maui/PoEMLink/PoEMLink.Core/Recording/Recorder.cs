using System.Globalization;
using System.Text;
using PoEMLink.Osl;

namespace PoEMLink.Recording
{
  /// <summary>記録の 1 マス (1 子機の 1 チャネル)</summary>
  /// <param name="Key">列を見分ける鍵 (UID とチャネル番号)</param>
  /// <param name="Header">列の見出し (子機名_チャネル名[単位])。最初に現れたときのものを使う</param>
  /// <param name="Value">値。読めなかったときは null</param>
  public readonly record struct RecordCell(string Key, string Header, OslValueType Type, double? Value, byte Status);

  /// <summary>
  /// 計測値を記録し、CSV に書き出す。
  /// 列は子機とチャネルごと。記録中に子機が増えたら列を右に足し、それより前の行は空欄にする。
  /// 抜けた子機の列は、値を空欄、状態を通信失敗 (12) とする。
  /// </summary>
  public sealed class Recorder
  {
    private sealed record Column(string Key, string Header, OslValueType Type);

    private readonly List<Column> _columns = new();
    private readonly Dictionary<string, int> _index = new();
    private readonly List<(DateTime Time, double?[] Values, byte[] Status)> _rows = new();
    private readonly object _sync = new();

    public int Count
    {
      get { lock (_sync) return _rows.Count; }
    }

    public DateTime? StartedAt { get; private set; }

    public void Clear()
    {
      lock (_sync)
      {
        _columns.Clear();
        _index.Clear();
        _rows.Clear();
        StartedAt = null;
      }
    }

    public void Add(DateTime time, IReadOnlyList<RecordCell> cells)
    {
      lock (_sync)
      {
        StartedAt ??= time;
        foreach (var c in cells)
        {
          if (_index.ContainsKey(c.Key)) continue;
          _index[c.Key] = _columns.Count;
          _columns.Add(new Column(c.Key, UniqueHeader(c.Header), c.Type));
        }
        var values = new double?[_columns.Count];
        var status = Enumerable.Repeat((byte)12, _columns.Count).ToArray();
        foreach (var c in cells)
        {
          int i = _index[c.Key];
          values[i] = c.Value;
          status[i] = c.Status;
        }
        _rows.Add((time, values, status));
      }
    }

    /// <summary>同じ見出しの子機があれば番号を付けて区別する</summary>
    private string UniqueHeader(string header)
    {
      if (_columns.All(c => c.Header != header)) return header;
      for (int n = 2; ; n++)
      {
        var h = $"{header} ({n})";
        if (_columns.All(c => c.Header != h)) return h;
      }
    }

    private static string Csv(string s) => s.IndexOfAny(new[] { ',', '"', '\n' }) < 0 ? s : "\"" + s.Replace("\"", "\"\"") + "\"";

    private static string Format(double v, OslValueType t) => t switch
    {
      OslValueType.Float32 => ((float)v).ToString(CultureInfo.InvariantCulture),
      _ => ((long)v).ToString(CultureInfo.InvariantCulture),
    };

    /// <summary>
    /// CSV を作る。列は Date, Time と、チャネルごとに「値」と「状態」(BACnet Reliability の値。0 = 正常)。
    /// 値は表示の桁で丸めず、子機が送った値をそのまま書く。
    /// </summary>
    public string ToCsv()
    {
      lock (_sync)
      {
        var sb = new StringBuilder();
        sb.Append("Date,Time");
        foreach (var c in _columns)
        {
          sb.Append(',').Append(Csv(c.Header));
          sb.Append(',').Append(Csv(c.Header + " status"));
        }
        sb.AppendLine();

        foreach (var (time, values, status) in _rows)
        {
          sb.Append(time.ToString("yyyy-MM-dd,HH:mm:ss", CultureInfo.InvariantCulture));
          for (int i = 0; i < _columns.Count; i++)
          {
            sb.Append(',');
            if (i < values.Length && values[i] is double v) sb.Append(Format(v, _columns[i].Type));
            sb.Append(',');
            if (i < status.Length) sb.Append(status[i]);
          }
          sb.AppendLine();
        }
        return sb.ToString();
      }
    }
  }
}
