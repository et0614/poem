using PoEMLink.Link;
using PoEMLink.Osl;
using PoEMLink.Simulation;

namespace PoEMLink.Core.Tests
{
  /// <summary>模擬の Link と子機を相手に、L3 と L4 を通しで確かめる</summary>
  public class SimulationTests
  {
    private static (SimLinkTransport Link, PoemLinkClient Client) Open(params ISimI2cDevice[] devs)
    {
      var link = new SimLinkTransport { Latency = TimeSpan.Zero };
      foreach (var d in devs) link.Attach(d);
      link.Connect();
      return (link, new PoemLinkClient(link));
    }

    private static async Task<LinkI2cBus> BusAsync(PoemLinkClient c) => new(c, await c.GetInfoAsync());

    [Fact]
    public async Task GetInfo()
    {
      var (_, c) = Open();
      var info = await c.GetInfoAsync();
      Assert.Equal(1, info.ProtocolVersion);
      Assert.Equal(256, info.MaxTransfer);
      Assert.Equal(512, info.MaxBatch);
      Assert.Equal(100, info.SpeedKHz);
      Assert.True(info.Features.HasFlag(LinkFeatures.Batch));
      Assert.Equal(16, info.Serial.Length);
    }

    [Fact]
    public async Task Scan_FindsOslDevicesAndSkipsForeign()
    {
      var (_, c) = Open(SimPresets.Temperature(0x10, "室内"), SimPresets.Velocity(0x11), new SimForeignDevice(0x44));
      var bus = await BusAsync(c);
      var r = await OslScanner.ScanAsync(bus);

      Assert.Equal(new byte[] { 0x10, 0x11 }, r.Devices.Select(d => d.Address));
      Assert.Equal(new byte[] { 0x44 }, r.Unknown);

      var t = r.Devices[0];
      Assert.Equal(0x0101, t.Identity.Model);
      Assert.Equal("室内", t.Name);
      Assert.Equal(new[] { "Temp", "RH" }, t.Channels.Select(ch => ch.Name));
      Assert.Equal("°C", t.Channels[0].UnitName);
      Assert.Equal(4, t.Channels[0].Precision);
      Assert.True(r.Devices[1].Channels[1].IsAuxiliary);
      Assert.True(r.Devices[1].CanIdentify);
      Assert.False(t.CanIdentify);
    }

    [Fact]
    public async Task Poll_WarmupThenValues()
    {
      var (_, c) = Open(SimPresets.Velocity(0x11));
      var dev = (await OslScanner.ScanAsync(await BusAsync(c))).Devices.Single();

      var s = await dev.PollAsync();
      Assert.NotNull(s);
      Assert.Equal(new byte[] { 64, 64 }, s!.Status);  // 予熱中は「準備中」
    }

    [Fact]
    public async Task PollMany_UsesBatchAndParses()
    {
      var devs = Enumerable.Range(0x10, 20).Select(a => (ISimI2cDevice)SimPresets.Temperature((byte)a)).ToArray();
      var (_, c) = Open(devs);
      var bus = await BusAsync(c);
      var found = (await OslScanner.ScanAsync(bus)).Devices;
      Assert.Equal(20, found.Count);

      // 20 台 × (3 + 14 B) = 340 B なので 1 回の BATCH に収まる。40 台なら分割される
      var raw = await bus.WriteReadManyAsync(found.Select(d => d.PollRequest).ToList());
      for (int i = 0; i < found.Count; i++)
        Assert.NotNull(found[i].ParsePoll(raw[i]));
    }

    [Fact]
    public async Task PollMany_SplitsOverMaxBatch()
    {
      var devs = Enumerable.Range(0x10, 40).Select(a => (ISimI2cDevice)SimPresets.FpcTemperature((byte)a)).ToArray();
      var (_, c) = Open(devs);
      var bus = await BusAsync(c);
      var found = (await OslScanner.ScanAsync(bus)).Devices;
      var raw = await bus.WriteReadManyAsync(found.Select(d => d.PollRequest).ToList());
      Assert.All(found.Select((d, i) => d.ParsePoll(raw[i])), Assert.NotNull);
    }

    [Fact]
    public async Task PollMany_MissingDeviceIsNull()
    {
      var a = SimPresets.Temperature(0x10);
      var b = SimPresets.Temperature(0x11);
      var (link, c) = Open(a, b);
      var bus = await BusAsync(c);
      var found = (await OslScanner.ScanAsync(bus)).Devices;
      link.Detach(b);
      var raw = await bus.WriteReadManyAsync(found.Select(d => d.PollRequest).ToList());
      Assert.NotNull(raw[0]);
      Assert.Null(raw[1]);
    }

    [Fact]
    public async Task SetName_Utf8()
    {
      var (_, c) = Open(SimPresets.Temperature(0x10));
      var dev = (await OslScanner.ScanAsync(await BusAsync(c))).Devices.Single();
      Assert.True(await dev.SetNameAsync("会議室 B 天井"));
      Assert.Equal("会議室 B 天井", await dev.ReadNameAsync());
      await Assert.ThrowsAsync<ArgumentException>(() => dev.SetNameAsync(new string('あ', 22)));  // 66 バイト
    }

    [Fact]
    public async Task SetAddress_MovesDevice()
    {
      var (_, c) = Open(SimPresets.DifferentialPressure(0x10));
      var bus = await BusAsync(c);
      var dev = (await OslScanner.ScanAsync(bus)).Devices.Single();
      string uid = dev.Identity.Uid;

      Assert.True(await dev.SetAddressAsync(0x22));
      var again = (await OslScanner.ScanAsync(bus)).Devices.Single();
      Assert.Equal(0x22, again.Address);
      Assert.Equal(uid, again.Identity.Uid);
    }

    [Fact]
    public async Task ChannelDescriptors_SplitReadOver256()
    {
      // 16 チャネルのチャネル記述は 258 B で、1 回の転送 (256 B) を超える
      var chs = Enumerable.Range(0, 16).Select(i => new SimChannel(62, OslValueType.Float32, $"T{i}", 4, OslChannelFlags.None)).ToArray();
      var slave = new SimOslSlave(0x30, 0x0102, "", 200, chs, OslCapability.ChannelAttributes, 0, (t, ch) => ((double)ch, (byte)0));
      var (_, c) = Open(slave);
      var dev = (await OslScanner.ScanAsync(await BusAsync(c))).Devices.Single();
      Assert.Equal(16, dev.Channels.Count);
      Assert.Equal("T15", dev.Channels[15].Name);

      var s = await dev.PollAsync();
      Assert.Equal(15.0, s!.Values[15]);
    }

    [Fact]
    public async Task ExtStatus()
    {
      var (_, c) = Open(SimPresets.AbsolutePressure(0x10));
      var dev = (await OslScanner.ScanAsync(await BusAsync(c))).Devices.Single();
      var st = await dev.ReadExtStatusAsync();
      Assert.NotNull(st);
      Assert.Equal("電源投入", st!.ResetCauseName);
    }

    [Fact]
    public async Task Batch_RejectsOverMaxBatch()
    {
      var (_, c) = Open(SimPresets.Temperature(0x10));
      var ops = Enumerable.Range(0, 3).Select(_ => BatchOp.Read(0x10, 200)).ToList();  // 600 B > 512
      var ex = await Assert.ThrowsAsync<LinkException>(() => c.BatchAsync(ops));
      Assert.Equal(LinkResult.BadLength, ex.Result);
    }

    [Fact]
    public async Task NotConnected_Throws()
    {
      var link = new SimLinkTransport();
      using var c = new PoemLinkClient(link);
      await Assert.ThrowsAsync<LinkException>(() => c.GetInfoAsync());
    }
  }
}
