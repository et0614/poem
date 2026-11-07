# PoEM システム仕様書 (原稿)

仕様書の原本は `src/` の md。Word (.docx) は md から毎回作り直す。

| パス | 内容 |
|---|---|
| `metadata.yaml` | 表紙の情報 (題名、版、日付) |
| `src/` | 本文。ファイル名の順につなげる (`00_front` 前付け、`10`〜`70` 第1部〜第7部、`90_appendix` 付録) |
| `figures/` | 図 (PNG・SVG)。md から `![説明](figures/xxx.png)` で参照する |
| `reference.docx` | Word の書式のひな形 (`make_reference.py` で作る) |
| `build.py` | Word を作る |
| `build/` | 出力 (git に入れない) |

## Word を作る

pandoc が必要 (`winget install JohnMacFarlane.Pandoc`)。python-docx も使う (`pip install python-docx`)。

```bash
python docs/spec/make_reference.py
```

```bash
python docs/spec/build.py
```

`make_reference.py` は書式のひな形を作るときだけ実行すればよい。

## 書き方の約束

- **内容は md で直す。Word を手で直さない** (次に作り直すと消える)。体裁は `reference.docx` で直す。
- 章は `#`、節は `##`。章と節には自動で番号が付く。部・付録・前付けの見出しには `{.unnumbered}` を付ける。
- 規定は「しなければならない / してはならない / することを推奨する / してもよい」で書き、理由や経緯は解説として分ける。
- `> 執筆予定:` で始まる引用は、まだ書いていない部分の覚え書き。書き終えたら消す。
- 決まっていないことは本文に書かず、付録 F (未決事項) に足す。
- 各ファイルの先頭の `{=openxml}` の段落は改ページ (部ごとに新しいページから始める)。消さない。
