# PoEM のソフトウェア

```
software/
├── PoEM.slnx            .NET のソリューション (src と tests のプロジェクトすべて)
├── PoEMServer.slnf      サーバと共通部だけを開く・ビルドする絞り込み (MAUI が無い環境向け)
├── src/
│   ├── PoEM.Common/     サーバとアプリで共通の表 (機種名・単位・状態の名前) と表示用 ID (22bit、16 進 6 桁) の計算
│   ├── PoEMServer/      MQTT ブローカ + BACnet ブリッジ。親機 (Pod) の MQTT v2 を BACnet に公開する
│   ├── PoEMLink.Core/   PoEM Link との通信、OSL v2 の解釈、記録と CSV、Link と子機の模擬
│   └── PoEMLink.App/    PoEM Link アプリ (iOS・Android・Windows、.NET MAUI)
├── tests/
│   └── PoEMLink.Core.Tests/  単体試験 (PoEM.Common と PoEMLink.Core)
└── tools/
    └── python/          子機の試験・設定の道具 (CP2112 / MCP2221 経由で OSL v2 の子機を読み書きする)
```

- 機種を足すときは `src/PoEM.Common/OslTables.cs` だけを直す (サーバとアプリの両方に反映される)。
- `tools/python` はソリューションに含めない。使い方は `tools/python/README.md`。

## ビルドと試験

```bash
dotnet test tests/PoEMLink.Core.Tests
```

```bash
dotnet build PoEMServer.slnf
```

```bash
dotnet publish src/PoEMServer/PoEMServer.csproj -c Release -o <配置先>
```

- アプリは `dotnet build src/PoEMLink.App -f net10.0-windows10.0.19041.0` (Android は `-f net10.0-android`、iOS は `-f net10.0-ios`)。
  詳しくは `src/PoEMLink.App/README.md`。
- サーバの設定 (`setting.ini`) と親機の番号の対応 (`PodMap.json`) は、実行ファイルと同じフォルダに置く。
