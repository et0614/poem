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
    /// 親機は OSL 共通レジスタ仕様 (SHASE 2026 第2報) に基づき、
    ///   - "nodes/config/&lt;hex&gt;" : 子機 1 台の固定情報 (DataCount/UnitType/Name)、SCAN 時 1 回
    ///   - "nodes/data/&lt;hex&gt;"   : 子機 1 台の計測値 (Status1/Seq/value[])、毎秒
    /// を publish する。本サーバはこれらを受け取り、子機ごとに動的に BACnet オブジェクトを
    /// 生成・更新する。
    /// </summary>
    internal class Program
    {
        #region クラス変数

        private static readonly Dictionary<string, string> initSettings = new();
        private static bool writeToConsole = true;
        private static PoEMDevice? poemDevice;

        #endregion

        static async Task Main(string[] args)
        {
            showTitle();

            if (!loadInitFile()) return;

            // BACnet ブリッジを起動 (UDP サーバはコンストラクタ内で開始される)
            poemDevice = new PoEMDevice(int.Parse(initSettings["bacport"]), initSettings["bacip"]);

            bool.TryParse(initSettings["console"], out writeToConsole);

            await RunMqttBroker();
        }

        private static void showTitle()
        {
            Console.WriteLine();
            Console.WriteLine("#########################################################################");
            Console.WriteLine("#                                                                       #");
            Console.WriteLine("#                  PoEMServer  version 1.0.0 (OSL)                      #");
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
                    if (topic.StartsWith("nodes/config/"))
                    {
                        var cfg = JsonSerializer.Deserialize<NodeConfigPayload>(payload);
                        if (cfg != null) poemDevice!.OnConfig(cfg);
                    }
                    else if (topic.StartsWith("nodes/data/"))
                    {
                        var data = JsonSerializer.Deserialize<NodeDataPayload>(payload);
                        if (data != null) poemDevice!.OnData(data);
                    }
                    // それ以外のトピックは無視 (ブローカとしては配送だけする)
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
