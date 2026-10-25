namespace PoEMServer.BACnet
{
    /// <summary>
    /// 親機に番号 (0, 1, 2, …) を振り、PoEMDevice のインスタンス番号の範囲を決める。
    ///
    /// 親機には初めてメッセージを受け取った順に番号を振る。親機のオブジェクトの範囲は
    /// 番号 × <see cref="PoEMDevice.POD_BLOCK"/> から始まる。
    /// 番号の対応は PodMap.json (実行ファイルと同じフォルダ) に保存し、サーバを再起動しても
    /// 同じ親機には同じ番号を割り当てる (BMS 側の登録が変わらないように)。
    /// 親機を入れ替えて番号を付け直したいときは、PodMap.json を編集してサーバを再起動する。
    /// </summary>
    internal class PodRegistry
    {
        private const string POD_MAP_FILE = "PodMap.json";

        /// <summary>インスタンス番号の上限 (4194302) に収まる親機の数</summary>
        public const int MAX_PODS = (int)(4194302 / PoEMDevice.POD_BLOCK);

        private readonly string mapPath;

        /// <summary>親機 ID → 番号 (PodMap.json に保存)</summary>
        private readonly Dictionary<string, int> indices = new();

        private readonly object syncLock = new();

        public PodRegistry()
        {
            mapPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, POD_MAP_FILE);
            LoadMap();
        }

        /// <summary>親機の番号を返す。初めての親機なら空いている最小の番号を振って保存する</summary>
        public int IndexOf(string podId)
        {
            lock (syncLock)
            {
                if (indices.TryGetValue(podId, out int index)) return index;

                var used = indices.Values.ToHashSet();
                index = 0;
                while (used.Contains(index)) index++;
                if (index >= MAX_PODS)
                    throw new InvalidOperationException($"親機は {MAX_PODS} 台までです (PodMap.json を整理してください)");

                indices[podId] = index;
                SaveMap();
                return index;
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
