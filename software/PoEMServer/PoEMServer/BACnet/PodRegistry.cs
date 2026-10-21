namespace PoEMServer.BACnet
{
    /// <summary>
    /// 親機ごとの BACnet Device (PoEMDevice) を管理する。
    ///
    /// 親機 1 台を 1 つの BACnet Device として公開する。親機には初めてメッセージを受け取った順に
    /// 番号 (0, 1, 2, …) を振り、Device インスタンス番号と UDP ポートをそこから決める:
    ///   Device インスタンス番号 = bacdevice + 番号
    ///   BACnet/IP の専用ポート   = bacport   + 番号
    /// 番号の対応は PodMap.json (実行ファイルと同じフォルダ) に保存し、サーバを再起動しても
    /// 同じ親機には同じ番号を割り当てる (BMS 側の登録が変わらないように)。
    /// </summary>
    internal class PodRegistry
    {
        private const string POD_MAP_FILE = "PodMap.json";

        private readonly string templatePath;
        private readonly uint deviceBase;
        private readonly int portBase;
        private readonly string localIp;
        private readonly string mapPath;

        /// <summary>親機 ID → 番号 (PodMap.json に保存)</summary>
        private readonly Dictionary<string, int> indices = new();

        /// <summary>親機 ID → Device</summary>
        private readonly Dictionary<string, PoEMDevice> devices = new();

        private readonly object syncLock = new();

        public PodRegistry(string templatePath, uint deviceBase, int portBase, string localIp)
        {
            this.templatePath = templatePath;
            this.deviceBase = deviceBase;
            this.portBase = portBase;
            this.localIp = localIp;
            mapPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, POD_MAP_FILE);
            LoadMap();
        }

        /// <summary>親機の Device を返す。初めての親機なら番号を振って作る。</summary>
        public PoEMDevice Get(string podId)
        {
            lock (syncLock)
            {
                if (devices.TryGetValue(podId, out PoEMDevice? dev)) return dev;

                if (!indices.TryGetValue(podId, out int index))
                {
                    index = indices.Count == 0 ? 0 : indices.Values.Max() + 1;
                    indices[podId] = index;
                    SaveMap();
                }

                uint instance = deviceBase + (uint)index;
                int port = portBase + index;
                dev = new PoEMDevice(podId, templatePath, instance, port, localIp);
                devices[podId] = dev;
                Console.WriteLine($"[INFO] pod {podId}: BACnet Device {instance} on port {port} (index {index}).");
                return dev;
            }
        }

        private void LoadMap()
        {
            try
            {
                if (!File.Exists(mapPath)) return;
                var map = System.Text.Json.JsonSerializer.Deserialize<Dictionary<string, int>>(File.ReadAllText(mapPath));
                if (map == null) return;
                foreach (var kv in map) indices[kv.Key] = kv.Value;
                Console.WriteLine($"[INFO] loaded {indices.Count} pod mapping(s) from {POD_MAP_FILE}.");
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[WARN] failed to load {POD_MAP_FILE}: {ex.Message}");
            }
        }

        private void SaveMap()
        {
            try
            {
                var opts = new System.Text.Json.JsonSerializerOptions { WriteIndented = true };
                File.WriteAllText(mapPath, System.Text.Json.JsonSerializer.Serialize(indices, opts));
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[WARN] failed to save {POD_MAP_FILE}: {ex.Message}");
            }
        }
    }
}
