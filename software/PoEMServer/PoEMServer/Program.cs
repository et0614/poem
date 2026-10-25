using MQTTnet.Diagnostics.Logger;
using MQTTnet.Server;
using PoEMServer.BACnet;
using System.Net;
using System.Text;
using System.Text.Json;

namespace PoEMServer
{
    /// <summary>
    /// PoEM 親機 (poem_pod) 用 MQTT ブローカ + BACnet ブリッジ。
    ///
    /// 親機は OSL 共通レジスタ v2 の子機を読み、MQTT v2 で publish する (NodePayloads.cs)。
    /// 本サーバはこれを 1 つの BACnet Device (PoEMDevice) として公開し、親機ごとのインスタンス番号の
    /// 範囲 (PodRegistry) の中に、子機ごとの BACnet オブジェクトを動的に生成・更新する。
    /// </summary>
    internal class Program
    {
        #region クラス変数

        private static readonly Dictionary<string, string> initSettings = new();
        private static bool writeToConsole = true;
        private static PoEMDevice? device;

        #endregion

        static async Task Main(string[] args)
        {
            showTitle();

            if (!loadInitFile()) return;

            // BACnet Device はサーバに 1 つ。親機はポートではなくオブジェクトのインスタンス番号の範囲で分ける。
            string template = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "PoEMDeviceStorage.xml");
            string bacip = initSettings["bacip"];
            if (!TryGetDeviceInstance(bacip, out uint deviceInstance)) return;
            int port = int.Parse(initSettings.GetValueOrDefault("bacport", "47808"));
            string name = initSettings.GetValueOrDefault("bacname", $"PoEM_Server_{deviceInstance}");
            device = new PoEMDevice(template, deviceInstance, name, port, bacip, new PodRegistry());
            Console.WriteLine($"BACnet Device {deviceInstance} \"{name}\" on {bacip}:{port}");

            bool.TryParse(initSettings["console"], out writeToConsole);

            await RunMqttBroker();
        }

        /// <summary>
        /// Device インスタンス番号。bacdevice が auto (または未設定) なら bacip の末尾の数
        /// (192.168.100.25 → 25)。現場では「サーバの IP の末尾 = Device ID」と決めている。
        /// </summary>
        private static bool TryGetDeviceInstance(string bacip, out uint instance)
        {
            string v = initSettings.GetValueOrDefault("bacdevice", "auto");
            if (!v.Equals("auto", StringComparison.OrdinalIgnoreCase))
            {
                if (uint.TryParse(v, out instance) && instance <= 4194302) return true;
                Console.WriteLine($"[ERROR] bacdevice={v} is not a valid BACnet device instance (0-4194302 or auto).");
                return false;
            }
            if (IPAddress.TryParse(bacip, out var ip) && ip.GetAddressBytes() is { Length: 4 } b)
            {
                instance = b[3];
                return true;
            }
            Console.WriteLine($"[ERROR] cannot derive the device instance from bacip={bacip}. Set bacdevice explicitly.");
            instance = 0;
            return false;
        }

        private static void showTitle()
        {
            Console.WriteLine();
            Console.WriteLine("#########################################################################");
            Console.WriteLine("#                                                                       #");
            Console.WriteLine("#                  PoEMServer  version 2.1.0 (OSL v2)                    #");
            Console.WriteLine("#                                                                       #");
            Console.WriteLine("#########################################################################");
            Console.WriteLine();
        }

        private static bool loadInitFile()
        {
            string path = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "setting.ini");
            if (!File.Exists(path))
            {
                Console.WriteLine("\"setting.ini\" is missing.");
                return false;
            }
            using StreamReader sReader = new(path);
            string? line;
            while ((line = sReader.ReadLine()) != null)
            {
                if (string.IsNullOrEmpty(line)) continue;
                if (line.StartsWith('#')) continue;

                int semi = line.IndexOf(';');
                if (semi >= 0) line = line.Remove(semi);

                string[] st = line.Split('=');
                if (st.Length < 2) continue;
                initSettings[st[0].Trim()] = st[1].Trim();
            }
            return true;
        }

        private static async Task RunMqttBroker()
        {
            bool dbg = false;
            bool.TryParse(initSettings.GetValueOrDefault("debug", "false"), out dbg);
            var mqttServerFactory = dbg ? new MqttServerFactory(new ConsoleLogger()) : new MqttServerFactory();

            var mqttServerOptions = new MqttServerOptionsBuilder()
                .WithDefaultEndpoint()
                .WithDefaultEndpointBoundIPAddress(IPAddress.Parse(initSettings["mqttip"]))
                .WithDefaultEndpointPort(1883)
                .Build();

            using var mqttServer = mqttServerFactory.CreateMqttServer(mqttServerOptions);

            // クライアント接続時: SessionItems に IP を保存
            mqttServer.ValidatingConnectionAsync += e =>
            {
                if (e.Endpoint != null)
                    e.SessionItems["IP_ADDRESS"] = e.Endpoint.Split(':')[0];
                return Task.CompletedTask;
            };

            // Publish 受信時: トピックで config / data を判別して BACnet へ反映
            mqttServer.InterceptingPublishAsync += e =>
            {
                string topic = e.ApplicationMessage.Topic;
                string payload = Encoding.UTF8.GetString(e.ApplicationMessage.Payload);
                string ip = (e.SessionItems["IP_ADDRESS"] as string) ?? "Unknown";

                if (writeToConsole)
                {
                    Console.WriteLine($"{DateTime.Now:HH:mm:ss}  {ip}  {e.ClientId}  {topic}  {payload}");
                }

                try
                {
                    Dispatch(topic, payload);
                }
                catch (JsonException jex)
                {
                    Console.WriteLine($"[ERROR] JSON parse failed for topic '{topic}': {jex.Message}");
                }
                catch (Exception ex)
                {
                    Console.WriteLine($"[ERROR] BACnet update failed for topic '{topic}': {ex.Message}");
                }

                return Task.CompletedTask;
            };

            await mqttServer.StartAsync();
            //Console.WriteLine("MQTT broker started. Press Enter to terminate.");
            //Console.ReadLine();
            Console.WriteLine("MQTT broker started.");

            // systemd 配下では stdin が端末ではないので Console.ReadLine() が即 null を返す。
            // SIGTERM (systemctl stop) と Ctrl-C の両方で抜けられるようにする。
            using var stopping = new ManualResetEventSlim(false);
            AppDomain.CurrentDomain.ProcessExit += (_, _) => stopping.Set();
            Console.CancelKeyPress += (_, e) => { e.Cancel = true; stopping.Set(); };
            stopping.Wait();
            await mqttServer.StopAsync();
        }

        /// <summary>
        /// トピックで振り分ける (MQTT v2):
        ///   poem/&lt;pod&gt;/pod/info | diag
        ///   poem/&lt;pod&gt;/node/&lt;adr&gt;/config | data | diag
        /// それ以外のトピックは無視する (ブローカとしては配送だけする)。
        /// </summary>
        private static void Dispatch(string topic, string payload)
        {
            string[] t = topic.Split('/');
            if (t.Length < 4 || t[0] != "poem") return;
            string podId = t[1];

            if (t[2] == "pod" && t.Length == 4)
            {
                if (t[3] == "info")
                {
                    var info = JsonSerializer.Deserialize<PodInfoPayload>(payload);
                    if (info != null) device!.OnPodInfo(podId, info);
                }
                return;     // pod/diag はログに出すだけ
            }

            if (t[2] != "node" || t.Length != 5 || !byte.TryParse(t[3], out byte adr)) return;
            switch (t[4])
            {
                case "config":
                    var cfg = JsonSerializer.Deserialize<NodeConfigPayload>(payload);
                    if (cfg != null) device!.OnConfig(podId, adr, cfg);
                    break;
                case "data":
                    var data = JsonSerializer.Deserialize<NodeDataPayload>(payload);
                    if (data != null) device!.OnData(podId, adr, data);
                    break;
                case "diag":
                    var diag = JsonSerializer.Deserialize<NodeDiagPayload>(payload);
                    if (diag != null) device!.OnDiag(podId, adr, diag);
                    break;
            }
        }

        #region Debug 用ロガー

        private class ConsoleLogger : IMqttNetLogger
        {
            private readonly object _consoleSyncRoot = new();

            public bool IsEnabled => true;

            public void Publish(MqttNetLogLevel logLevel, string source, string message, object[]? parameters, Exception? exception)
            {
                var fg = ConsoleColor.White;
                switch (logLevel)
                {
                    case MqttNetLogLevel.Verbose: fg = ConsoleColor.White; break;
                    case MqttNetLogLevel.Info:    fg = ConsoleColor.Green; break;
                    case MqttNetLogLevel.Warning: fg = ConsoleColor.DarkYellow; break;
                    case MqttNetLogLevel.Error:   fg = ConsoleColor.Red; break;
                }

                if (parameters?.Length > 0) message = string.Format(message, parameters);

                lock (_consoleSyncRoot)
                {
                    Console.ForegroundColor = fg;
                    Console.WriteLine(message);
                    if (exception != null) Console.WriteLine(exception);
                    Console.ResetColor();
                }
            }
        }

        #endregion
    }
}
