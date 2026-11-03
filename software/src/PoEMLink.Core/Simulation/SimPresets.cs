using PoEMLink.Osl;

namespace PoEMLink.Simulation
{
  /// <summary>
  /// PoEM の子機の模擬。チャネル・周期・機能は各ファームウェアの main.c と同じにしてある。
  /// 値はもっともらしい範囲でゆっくり揺らす。
  /// </summary>
  public static class SimPresets
  {
    private const OslCapability Basic = OslCapability.ChannelAttributes | OslCapability.ExtStatus | OslCapability.CmdReset;

    private static double Wave(double t, double period, double amp) => amp * Math.Sin(2 * Math.PI * t / period);

    private static (double, byte) Ok(double v) => (v, (byte)0);

    /// <summary>温湿度 (model 0x0101、poem_temperature_sensor.X)</summary>
    public static SimOslSlave Temperature(byte addr, string name = "") => new(addr, 0x0101, name, 125,
      new[]
      {
        new SimChannel(62, OslValueType.Float32, "Temp", 4, OslChannelFlags.None),
        new SimChannel(29, OslValueType.Float32, "RH", 4, OslChannelFlags.None),
      },
      Basic, 1000,
      (t, ch) => ch == 0 ? Ok(24.3 + Wave(t, 120, 0.4) + Wave(t, 7, 0.02)) : Ok(48.0 + Wave(t, 90, 2.0)));

    /// <summary>多点温度 (model 0x0102、FPC_TSensor.X)</summary>
    public static SimOslSlave FpcTemperature(byte addr, string name = "") => new(addr, 0x0102, name, 200,
      new[]
      {
        new SimChannel(62, OslValueType.Float32, "T1", 5, OslChannelFlags.None),
        new SimChannel(62, OslValueType.Float32, "T2", 5, OslChannelFlags.None),
        new SimChannel(62, OslValueType.Float32, "T3", 5, OslChannelFlags.None),
      },
      Basic | OslCapability.CmdIdentify, 200,
      (t, ch) => Ok(22.0 + 0.8 * ch + Wave(t + 10 * ch, 60, 0.3)));

    /// <summary>風速 (model 0x0201、poem_velocity_sensor.X)。起動後 5 秒は予熱で「準備中」</summary>
    public static SimOslSlave Velocity(byte addr, string name = "") => new(addr, 0x0201, name, 500,
      new[]
      {
        new SimChannel(74, OslValueType.Float32, "Vel", 3, OslChannelFlags.None),
        new SimChannel(5, OslValueType.Float32, "Volt", 4, OslChannelFlags.Auxiliary),
      },
      Basic | OslCapability.CmdIdentify, 5000,
      (t, ch) =>
      {
        double v = Math.Max(0, 0.25 + Wave(t, 15, 0.12) + Wave(t, 2.3, 0.04));
        return ch == 0 ? Ok(v) : Ok(1.35 + 0.6 * Math.Sqrt(v));
      },
      avgWindowMs: 0);  // 平均ではなく指数平滑 (filter_n)

    /// <summary>絶対圧 (model 0x0301、poem_absolute_pressure_sensor.X)</summary>
    public static SimOslSlave AbsolutePressure(byte addr, string name = "") => new(addr, 0x0301, name, 125,
      new[]
      {
        new SimChannel(53, OslValueType.Float32, "Press", 6, OslChannelFlags.None),
        new SimChannel(62, OslValueType.Float32, "Temp", 4, OslChannelFlags.Auxiliary),
      },
      Basic, 1000,
      (t, ch) => ch == 0 ? Ok(101325 + Wave(t, 300, 30) + Wave(t, 3, 1.5)) : Ok(25.1 + Wave(t, 200, 0.2)));

    /// <summary>差圧 (model 0x0302、poem_differential_pressure_sensor.X)。範囲 ±125 Pa を超えると「上限超過」</summary>
    public static SimOslSlave DifferentialPressure(byte addr, string name = "") => new(addr, 0x0302, name, 125,
      new[]
      {
        new SimChannel(53, OslValueType.Float32, "DP", 4, OslChannelFlags.None),
        new SimChannel(62, OslValueType.Float32, "Temp", 4, OslChannelFlags.Auxiliary),
      },
      Basic, 1000,
      (t, ch) =>
      {
        if (ch == 1) return Ok(24.8 + Wave(t, 200, 0.2));
        double dp = 12.5 + Wave(t, 40, 6.0) + Wave(t, 1.7, 0.3);
        return (dp, (byte)(dp > 125 ? 2 : 0));
      });

    /// <summary>アプリの模擬モードで使う構成: PoEM の子機 4 台と、OSL ではない機器 1 台</summary>
    public static SimLinkTransport CreateDemo()
    {
      var link = new SimLinkTransport();
      link.Attach(Temperature(0x10, "温湿度 室内"));
      link.Attach(Velocity(0x11, "風速 吹出口"));
      link.Attach(DifferentialPressure(0x12, "差圧 フィルタ前後"));
      link.Attach(FpcTemperature(0x13, "表面温度 窓"));
      link.Attach(new SimForeignDevice(0x44));
      return link;
    }
  }
}
