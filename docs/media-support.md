# メディア対応の設計検討 — OpenEXR / 動画再生 / click-to-open UX

将来判断のための調査メモ(LOW PRIORITY、実装なし)。前提: C++17 / FetchContent /
重い依存を嫌う([CMakeLists.txt](../CMakeLists.txt))、ImageDoc は float32 plane
1〜4ch([core/main.cpp](../core/main.cpp) `struct ImageDoc`)、stack = 時間解析の単位、
remote は npy のみ配信([core/serve.cpp](../core/serve.cpp))。

> **§1 (どのライブラリで画像形式を読むか) は
> [docs/media-formats.md](media-formats.md) に移った。**
> あちらは調査メモではなく**測定つきの判断**で、EXR だけでなく PNG やその他の
> 形式、そして「その画素は測定値か」という問題まで扱う。
> この文書に残っているのは **動画 (§2)** と **click-to-open UX (§3)** である。

---

## 1. OpenEXR 対応 → [docs/media-formats.md](media-formats.md) に移動

**この節の結論 (tinyexr 推奨) は破棄された。ここには記録だけ残す。**

初版のこの節は候補を3つ (公式 OpenEXR / tinyexr / 自前最小リーダ) 比較して
**tinyexr を推した** — 単一ヘッダで、`TINYEXR_USE_MINIZ` により既に
FetchContent 済みの miniz を再利用でき、公式 lib は「Imath ごと fetch して
4 ライブラリを建てるのは repo の流儀に対して重い」と考えたからである。

**ユーザーがこれを覆し、公式 OpenEXR を選んだ。**
その記録が [docs/tasks.csv](tasks.csv) の「フォーマット」行
(`OpenEXR(公式lib)/動画(ffmpeg)/ベタRAWリモート/params_schema`) である。

そして「重い」という当時の判断は**推測であって測定ではなかった**。
実際に測ると configure +17.8 s / build +20.3 s / バイナリ +2.98 MiB で、
Imath は OpenEXR が自分で取り、libdeflate は vendored 済みだった。

**現在の判断は [docs/media-formats.md](media-formats.md) にある。**
あちらが決めていること (ここでは繰り返さない):

- 3つの戦略 (OIIO 全部 / 形式ごとの公式 lib / 狭く + アダプタ) の測定つき比較
- **OIIO は FetchContent だけでは建たなかった**という実測と、その意味
- **画素が [DN] かどうかを名乗る `values` フィールド**と、analyzer の門番
  — PNG/TIFF/JPEG のような「自分が何かを言わないファイル」を
  黙って測定値として扱わないための仕組み
- **`Imf::setGlobalThreadCount()` を呼ばないと 3.6〜5.0 倍遅い**という実測
  (「OIIO の方が読み出しが速い」という印象のおそらくの正体)
- native reader とアダプタの境界を**どこに引くか**

なお、この節にあった **loadExr の挿入点** (`loadNpy` / `loadRaw` の並び)、
**channel → ImageDoc のマッピング** (`R,G,B(,A)`→ch=3/4、単独 AOV→ch=1、
multi-layer は npz member と同じ扱い、chroma subsampled は当面未対応)、
**ロード時トーンマップはしない**、**remote は f32 TILE でプロトコル変更ゼロ**
(ただし loader が main.cpp にある間は serve から呼べないので**まずローカル専用**)
という設計は**そのまま生きている**。media-formats.md はそれを前提にしている。

---

## 2. 動画再生(再生だけ)

IQ エンジニアが動画にすること = scrub / frame step / A/B 比較 / per-frame stats。
これは**既存の stack モデルそのもの**(動画 = 遅延デコードされる stack)。
リアルタイム再生・音声は要件ではない。

| 案 | 評価 |
|---|---|
| (a) libav を FetchContent | 巨大(ビルド時間・バイナリとも)。静的リンクは LGPL 義務(再リンク手段の提供)+ GPL 部品混入リスク。この repo には過剰 |
| (b) OS デコーダ (MF / AVFoundation) | OS 毎に別実装 ×2、**Linux に可搬な話がない**。remote peer は Linux 計算機なので致命的 |
| (c) pl_mpeg 系単一ヘッダ | MIT・約 3000 行で見事だが **MPEG1 限定**。実務の H.264/265 が開けない。玩具止まり |
| (d) **PATH 上の ffmpeg にパイプ** | リンク時依存ゼロ、ライセンス義務なし(別プロセス)、無ければ機能が消えるだけ。pfd が zenity に shell out するのと同じ、この repo の流儀 |

**推奨: (d) ffmpeg-pipe。**

- Phase 1: `ffprobe -print_format json` でメタ取得 → `ffmpeg -i file -f rawvideo
  -pix_fmt <fmt> -` を stdout パイプで読み、**N フレームを普通の stack として登録**。
  N は既存の memory budget(`seqMemBudget`)がそのまま上限になる。scrub / step /
  A/B / Temporal / per-frame stats は追加実装ゼロで動く。
- pix_fmt は測定再現性のため明示固定する(8bit は `rgb24`→f32、10bit 系は
  `gbrpf32le` 等)。「codec を通った値の測定」である旨と、色変換・range 指定を
  Inspector の note に記録する。
- ffmpeg 不在時: 拡張子は見えるが「ffmpeg が PATH にありません」と言って開かない。
  静かに劣化、依存は増えない。
- Phase 2(必要になったら): `-ss` シークで budget 窓をスライドする遅延デコード。
  remote は serve 側で同じ ffmpeg-pipe を張り f32 TILE を返す形に自然に伸びる。

---

## 3. click-to-open UX(preview と登録 open の分離)

現状: browser / Remote パネルの 1 クリック = 登録 open(Files に入り、remote なら
step>1 preview + **裏でフル解像度 fetch まで走る**)。メディア閲覧には重い。

他ツールの解法: VSCode = single-click で斜体の transient tab(1 枠を再利用、編集で
pin)/ FastStone・IrfanView = ブラウズペイン自体が viewer / macOS Quick Look =
Space で消える preview。共通原理は「**見るだけの状態を安く、所有する操作で昇格**」。

本ツールへの提案:

- **preview slot を 1 つ**持つ(`ImageDoc` に `transient` フラグ、Files には
  斜体・淡色で最上段に 1 行だけ出す)。single-click はこの slot を**置き換える**。
  ESC で閉じる。session に保存しない。
- **昇格 = 所有を示す操作**: double-click / Enter / ROI・POI を置く / analyzer を
  走らせる / compare B に選ぶ / session 保存 — どれかで transient フラグが外れ
  通常 open になる(VSCode の「編集で pin」に対応。**測ったものは記録に残る**、
  という本ツールの性格に合う)。
- remote との相互作用が一番おいしい: preview は既存の step>1 fetch **だけ**で止め、
  フル解像度の背景 fetch は昇格まで遅延。現状「クリックのたびフル 1 枚転送」が消える。
- stack / group: preview は先頭 1 フレームのみ。昇格で従来の budget 付き prefetch。
  動画: preview は poster frame 1 枚。
- 移行リスク: 「1 クリック = open」に馴染んだ手が誤爆する。対応:
  1. まず **Space = Quick Look 風 preview を追加するだけ**(クリック挙動不変、
     リスクゼロ)で価値検証。
  2. 次リリースで設定 `Browser click: preview first (new) / open immediately
     (classic)` を追加、classic 既定 + 初回 hint toast。
  3. 定着を見て new を既定に反転。double-click/Enter はどのモードでも登録 open。

---

## 推奨フェージング(GO が出た場合)

| Phase | 内容 | 規模感 |
|---|---|---|
| **A0** | **`values` フィールド + analyzer の門 + selftest 1本** ([media-formats.md](media-formats.md) §5) | S |
| A | **公式 OpenEXR** で loadExr(ローカルのみ、scanline RGB/Y/layer→member)。**`setGlobalThreadCount()` を必ず呼ぶ** | S(数日) |
| B | preview slot UX(Space preview → click 設定 → 既定反転の 3 段階) | M(1〜2 週) |
| C | 動画 Phase 1: ffmpeg-pipe → N フレーム stack(budget 適用) | M(1〜2 週) |
| D | loader 共有 TU 化 + serve 側 EXR(f32 TILE、プロトコル不変) | S〜M |
| E | 動画の窓デコード / remote 動画 | L(必要が立証されてから) |

A0→A→B→C の順を推す: **A0 は A より先** — 表示参照の画素が入ってくる前に
門を作るのが一番安い([media-formats.md](media-formats.md) §5)。
A は独立で安い。B は C の前提(動画こそ preview が要る)。

**D には CI の代償がある**: serve 側に EXR を入れると、Linux ジョブが
`viewer-serve` を Ubuntu 20.04 コンテナで**手書きの g++ 1行**で建て直している
箇所(`.github/workflows/build.yml`:90-108、cmake は入っていない)が破綻する。
[media-formats.md](media-formats.md) §1.3 / §3.6 を読んでから着手すること。

参考: [OpenEXR install(Imath/libdeflate)](https://openexr.com/en/latest/install.html) /
[OpenEXR license](https://openexr.com/en/latest/license.html) /
[pl_mpeg](https://github.com/phoboslab/pl_mpeg) /
~~[tinyexr](https://github.com/syoyo/tinyexr)~~(§1 のとおり不採用)
