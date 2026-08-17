現行ドキュメント: [media-support.md](../../media-support.md) の背景 — EXR リーダの候補比較と、ユーザーが覆した tinyexr 推奨。

# メディア対応の設計検討 — 背景

移動元: docs/media-support.md (2026-08-17、issue #55 の第二段分割)。
**本文は1文字も変えていない** — 節をそのまま切って移しただけである。
この節の推奨 (tinyexr) は #53 でユーザーが覆し、公式 OpenEXR になった。
生きている判断 (channel マッピング・トーンマップなし・layer=npz member) と
実測コストは現行側の §1 にある。

---

### 候補比較

| | 公式 OpenEXR 3.x | tinyexr | 自前最小リーダ |
|---|---|---|---|
| 形態 | CMake ライブラリ群 (OpenEXRCore/OpenEXR/Iex/IlmThread) | 単一ヘッダ C++ | 数百行 |
| 依存 | **Imath**(無ければ自動 fetch)+ libdeflate(3.4 で vendored) | miniz か zlib を選択 | miniz のみ |
| ライセンス | BSD-3-Clause | BSD-3-Clause | — |
| half→f32 | Imath::half(基準実装) | 内蔵ビット展開 | 自前 ~20 行 |
| 圧縮 | 全方式 (ZIP/PIZ/DWA/…) | ZIP/ZIPS/RLE/PIZ/ZFP 等 | NONE+ZIP が現実的な上限 |
| fuzzing | OSS-Fuzz 常時(CVE 多数→修正済の歴史) | 0.9.5 期に CVE 複数(heap overflow 等)、v1.0.7 で fuzz 済・既知クラッシュなしと明言 | 自分で fuzz する羽目になる |
| 保守 | ASWF、活発 | syoyo、活発(C11 書き直し `exr.h` が次期主線) | 自分 |

要点:

- **half→float32 は情報無損失**(全 half 値が f32 で表現可能)。「変換の正しさ」は丸めでなく
  denormal / Inf / NaN の展開バグの有無の問題で、tinyexr の実装で足りる。測定値の正確さは
  どの候補でも損なわれない。
- 自前リーダは魅力的に見えるが、実務の EXR は PIZ / DWA 圧縮が普通に混ざる。
  「NONE+ZIP だけ読める」は開けないファイルへの不満を量産する。**却下**。
- 公式 lib は品質最高だが、Imath ごと fetch して 4 ライブラリをビルドするのは
  この repo の流儀(miniz を 1 ファイル直コンパイル)に対して重い。scanline 読みだけに
  払う代価ではない。
- **推奨: tinyexr**。単一ヘッダ、BSD-3、そして `TINYEXR_USE_MINIZ` で
  **既に FetchContent 済みの miniz をそのまま再利用できる**(include パスを
  `${miniz_SOURCE_DIR}` に向けるだけ。miniz.c の二重コンパイル・シンボル衝突に注意)。
  CVE 歴は「単独ヘッダの画像パーサの通例」レベルで、入力は自分のラボのファイルが主。
