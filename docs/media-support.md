# メディア対応の設計検討 — OpenEXR / 動画再生 / click-to-open UX

将来判断のための調査メモ(LOW PRIORITY、実装なし)。前提: C++17 / FetchContent /
重い依存を嫌う([CMakeLists.txt](../CMakeLists.txt))、ImageDoc は float32 plane
1〜4ch([core/main.cpp](../core/main.cpp) `struct ImageDoc`)、stack = 時間解析の単位、
remote は npy のみ配信([core/serve.cpp](../core/serve.cpp))。

---

## 1. OpenEXR 対応

必要なのは scanline RGB/Y の half/float を**画素値そのまま**(トーンマップなし)で
読むことだけ。deep / tiled / multipart は当面対象外。

> **決定 (2026-08-03): 公式 OpenEXR を採用。実装済み (`media-openexr`)。**
> 以下の比較検討は tinyexr を推奨していたが、**ユーザーがこれを覆し**、
> ASWF 公式 OpenEXR を使うと決めた。**この節の他の分析はすべて有効**である —
> channel マッピング、挿入点、multi-layer を npz member 相当として扱う設計、
> remote への波及(案 a: まずローカル専用)、そして「ロード時にトーンマップは
> **しない**」— いずれもそのまま実装した。変わったのは**提供元だけ**で、
> 「何を読むか」「どう見せるか」は変わっていない。
> 覆された理由は下の「却下理由の再評価」に、実測コストは「実測コスト」に置く。

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
- ~~**推奨: tinyexr**~~。単一ヘッダ、BSD-3、そして `TINYEXR_USE_MINIZ` で
  **既に FetchContent 済みの miniz をそのまま再利用できる**(include パスを
  `${miniz_SOURCE_DIR}` に向けるだけ。miniz.c の二重コンパイル・シンボル衝突に注意)。
  CVE 歴は「単独ヘッダの画像パーサの通例」レベルで、入力は自分のラボのファイルが主。
  → **却下 (ユーザー判断)**。

### 却下理由の再評価 — なぜ「重い」が決め手にならなかったか

上の表で公式 lib を退けた根拠は **「4 ライブラリをビルドするのは重い」** の 1 点
だけだった。これを実測したところ、**その前提が数字として弱い**ことが分かった
(下記)。加えて、この表が tinyexr 側に置いた利点のうち 2 つは、測定ツールとして
見ると利点ではない:

- **fuzzing**: 上の表自身が「tinyexr は CVE 複数」「公式は OSS-Fuzz 常時」と書いて
  いる。入力が自ラボのファイル中心でも、**壊れた EXR は日常的に発生する**
  (転送断・書き込み中のファイル)。パーサが落ちる/踏み抜くかどうかは
  ライセンスや行数より重い。
- **half→float の基準実装**: 「どちらでも測定値は損なわれない」は正しいが、
  **正しさの立証責任**が違う。Imath は基準実装そのもので、denormal/Inf/NaN の
  展開が定義どおりであることを自分で証明しなくてよい。測定ツールでは
  「たぶん合っている」と「基準実装である」の差は大きい。

つまり公式 lib のコストは**ビルド時間とバイナリサイズという測れるもの**で、
tinyexr のコストは**パーサの堅牢性という測りにくいもの**だった。前者を測って
許容範囲だったので、後者を引き受ける理由が無くなった。

### 実測コスト (MinGW/GCC 16.1, Ninja, Release, -j8, 本 repo で実測)

| | ベースライン | +OpenEXR | 差 |
|---|---|---|---|
| configure (cold, DL 込み) | 4 s | 14 s | **+10 s** |
| build (cold) | 69 s | 86 s | **+17 s** |
| 合計 (cold) | 73 s | 100 s | **+27 s (+37%)** |
| `viewer.exe` | 7,996,319 B | 11,223,727 B | **+3,227,408 B (+3.08 MiB, +40%)** |

- 「CI 時間が倍になる」ようなことは**起きていない** — cold で +27 s。
  build.yml の 1 ジョブは fixture 生成 + 22 selftest + bench を含むので、
  **ジョブ全体に対しては 1 割未満**。3 OS × 1 回なので matrix 構造は不変。
- ビルドされるのは **Imath / Iex / IlmThread / OpenEXR / OpenEXRCore の 5 つだけ**。
  `EXCLUDE_FROM_ALL`(CMake 3.28+)で tools / examples / tests / OpenEXRUtil を
  既定ターゲットから外しているため。3.28 未満では OpenEXRUtil も建つ(+数十秒)。
- バイナリ +3 MiB のうち**一部は書き込み側**である。selftest V23 が fixture を
  自前で書くため `ImfOutputFile` 等がリンクされる。**アプリは読むだけ**なので、
  これは「テストのために出荷バイナリが太っている」という素直に不本意な代価で、
  レビューで議論する価値がある(代案は §「fixture をどこで作るか」)。
- **libdeflate と OpenJPH は vendored**。外部 zlib を要求しないので、
  この repo の「重い依存を嫌う」方針との衝突は思ったより小さい。

### 配線 (CMakeLists.txt)

```
option(VIEWER_WITH_EXR "..." ON)     # 既定 ON
  find_package(OpenEXR 3.1 QUIET)    # 1) 入っていればそれを使う
  → 無ければ FetchContent (URL 固定 pin: OpenEXR v3.4.13 / Imath v3.2.2)
```

- **`find_package` 先行**の理由: distro / vcpkg / brew が既に OpenEXR を持って
  いるなら、packager に同じライブラリを二重に落とさせる理由がない。
- **URL pin** は本 repo の流儀(glfw / imgui / miniz / pfd と同じ)。OpenEXR 自身の
  Imath 自動取得は `GIT_REPOSITORY` を使うため git を要求し、pin の流儀も違う。
  よって **Imath はこちらで先に宣言**する(OpenEXR は `TARGET Imath::Imath` を
  見てから取りに行くので、先に置けば勝つ)。
- **オフライン**: 既存 4 つと同じ override が効く。追加された名前は
  **`FETCHCONTENT_SOURCE_DIR_IMATH`** と **`FETCHCONTENT_SOURCE_DIR_OPENEXR`**。
- **ネットワークが無い人**: `-DVIEWER_WITH_EXR=OFF` で従来どおり建つ。その
  バイナリは `.exr` を渡されると
  `built without OpenEXR support (configure with -DVIEWER_WITH_EXR=ON)` と
  **名指しで断る** — 黙って無視も、偽の画像も作らない。
- `viewer-serve` は**リンクしない**。remote は依然 npy のみ配信(案 a)であり、
  peer は OpenGL も無い計算機で建つ必要があるため、依存を増やさない。

### 読めるもの / 断るもの (実装済み)

読める: 単一 part の **scanline**、**half (`f16`) / float (`f32`)**、
`R,G,B(,A)` → 3/4ch、単独 `Y` や単独 AOV → 1ch、multi-layer は prefix で
グループ化して **1 layer = 1 doc**。dtype は `f16`/`f32` を記録し Inspector に出る。
表示レンジはデータ自身の min/max から。**トーンマップ・γ・クランプは一切しない。**

断るもの(すべて**名指し＋理由**、`docs/import-adapters.md` §3.2 の作法):

| 対象 | 文言 |
|---|---|
| chroma subsample | `chroma-subsampled channel 'BY' (sampling 2x2): only full-resolution channels are read, there is no chroma reconstruction here` |
| tiled | `tiled EXR: only scanline files are read` |
| deep | `deep EXR: only flat scanline files are read (a deep pixel is a list of samples, not a value)` |
| multi-part | `multi-part EXR (N parts): only single-part scanline files are read` |
| UINT channel | `channel 'X' is UINT: only half and float channels are read` |
| EXR 無効ビルド | `built without OpenEXR support (configure with -DVIEWER_WITH_EXR=ON)` |

### 決めた細部 (レビューで見てほしい判断)

- **layer は `FrameSource::npzMember` に載せた**。新しい並行フィールドを作らず、
  session の保存/復元・Inspector 表示・`--compare` の同一性判定をそのまま
  再利用している(「1 つのコンテナ内の名前付き部分配列」という意味は同一)。
  名前が npz 寄りなのが唯一の難点。
- **`R,G,B,A` 以外の並びは 1ch ずつに割る**。`X,Y,Z` を「色」として 3ch に
  詰めるのは形式が定義していない推測になるため。
- **1 ファイル = 1 frame**。frame 軸は捏造しない。フォルダに並んだ `.exr` が
  既存の sequence 機構で stack になる(`SEQ_EXTS` に `.exr` を追加。併せて
  `isRaw = ext != ".npy"` を `.exr` も除外するよう直した — さもないと
  形を自分で持っている EXR に対して RAW 寸法ダイアログが出る)。
- **Browse パネルは対象外**。あそこの「画像か?」判定は `isNpyName` で、remote
  peer が npy しか配信できないことと結びついている。ここに `.exr` を足すと
  サーバが応えられない行を出すことになるので、触っていない。

### fixture をどこで作るか

`tools/testdata` は生成物(gitignore・CI が毎回再生成)なので EXR も生成する
必要があるが、**Python から EXR を書くには repo が持たない依存が要る**
(CI は numpy しか入れない)。そこで **selftest 内の C++ で書いている** — V21 が
miniz を使って .npz のバイトを test 内で組むのと同じ理由・同じ形。
代償は上記のバイナリ +α と、「読み手と書き手が同じプロジェクト」であること
(バイト列レベルの他人の EXR を parse する証明にはならない)。

### loadExr の挿入点と channel マッピング

- `loadNpy` / `loadRaw` の並び(main.cpp ~1653 行付近)に `loadExr(path)` を置く。
  `readFileBytes` → `ParseEXRHeaderFromMemory` + `DecodeEXRImage`(高レベル `LoadEXR` は
  常に RGBA 詰め替えをするので使わない — channel 名を保ちたい)。
- channel → ImageDoc(1〜4ch):
  - `R,G,B(,A)` → ch=3/4、`Y` 単独 → ch=1、`Z` 等の単独 AOV → ch=1。
  - 任意 AOV / multi-layer(`diffuse.R` 等)は **npz member と同じ扱い**:
    prefix でグループ化し、1 グループ = 1 doc、`npzMember` 相当のフィールドで
    session 復元。UI 前例(npz の複数 array)がそのまま使える。
  - `Y/RY/BY`(chroma subsampled)は初期は「未対応」でエラー文言を出す。
- 表示: データは scene-linear のまま `data` に入れ、black/white 初期値は
  データ min/max(既存の表示レンジ + gamma で見る。ロード時トーンマップは**しない**)。
  `dtype` は "f16"/"f32" を記録し、Inspector で由来が分かるようにする。

### remote への波及

TILE は dtype 付きで画素を運ぶ設計([remote_proto.h](../core/remote_proto.h))なので、
サーバ側で EXR→f32 に落として `DT_F32` を返せば**プロトコル変更ゼロ**(案 b)。
ただし loader が main.cpp 内にある現状では serve.cpp から呼べない。
**推奨: まず (a) ローカル専用**で出し、npy/exr デコードを `core/loaders.cpp` 的な
共有 TU へ切り出す整理(toFloatSamples と同じ共有パターン)ができた時点で (b) を足す。

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
| A | ~~tinyexr で~~ **公式 OpenEXR で** loadExr(ローカルのみ、scanline RGB/Y/layer→member) — **完了** | S(数日) |
| B | preview slot UX(Space preview → click 設定 → 既定反転の 3 段階) | M(1〜2 週) |
| C | 動画 Phase 1: ffmpeg-pipe → N フレーム stack(budget 適用) | M(1〜2 週) |
| D | loader 共有 TU 化 + serve 側 EXR(f32 TILE、プロトコル不変) | S〜M |
| E | 動画の窓デコード / remote 動画 | L(必要が立証されてから) |

A→B→C の順を推す: A は独立で安い。B は C の前提(動画こそ preview が要る)。

参考: [OpenEXR (採用)](https://github.com/AcademySoftwareFoundation/openexr) /
[Imath](https://github.com/AcademySoftwareFoundation/Imath) /
[tinyexr (却下)](https://github.com/syoyo/tinyexr) /
[tinyexr CVE 一覧](https://www.cvedetails.com/product/46936/Tinyexr-Project-Tinyexr.html?vendor_id=18510) /
[OpenEXR install(Imath/libdeflate)](https://openexr.com/en/latest/install.html) /
[OpenEXR license](https://openexr.com/en/latest/license.html) /
[pl_mpeg](https://github.com/phoboslab/pl_mpeg)
