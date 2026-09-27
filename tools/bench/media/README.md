# bench/media — 形式リーダのビルドコストと読み出し速度を測るハーネス

**これは `viewer` の一部ではない。** ルートの
[CMakeLists.txt](../../../CMakeLists.txt) はこのディレクトリを一切参照しておらず、
`viewer` / `viewer-serve` / plugins のどれにもリンクされない。selftest でもない。
**別プロジェクトとして自分で configure したときにだけ**建つ。
装置であって製品ではないので、意図的に CMake に繋いでいない。

## 何を裏づけた測定か

出荷済みの形式判断のうち、次の3つの**数字の出どころ**がここにある。

| 出荷済みの判断 | この装置が出した数字 |
|---|---|
| EXR は tinyexr ではなく**公式 OpenEXR** で読む | `exr/` の FetchContent 構成が実際に建つこと、および 4K float RGB の展開時間 |
| **OIIO で全形式を賄う案を採らない** | `oiio/` が同じ環境で建たなかったこと(最小構成にしてもなお)= 依存の代価の上限ではなく下限 |
| **ベンダ RAW は EXR より安く、PNG より正直**(RAW を native に入れ、黒を引かない) | `raw/` の `unpack()` 時間と、1画素も復号せずに非可逆性を判定できること(`get_decoder_info()` / `NEFCompression` / `canon.Quality`) |

## 結論はどこにあるか

| 文書 | 何が載っているか |
|---|---|
| [docs/features/media/media-support.md](../../../docs/features/media/media-support.md) | 現行仕様。§1 が OpenEXR 採用の経緯(公式 OpenEXR に決まった理由と tinyexr を覆した判断) |
| [docs/background/media/media-support.md](../../../docs/background/media/media-support.md) | 候補比較と、採用しなかった tinyexr 案の記録 |
| [docs/features/media/video-support.md](../../../docs/features/media/video-support.md) | 依存の代価を実測で扱った隣接判断。§4.2 がバイナリサイズ |
| [docs/features/adapters/input-adapters.md](../../../docs/features/adapters/input-adapters.md) | 「なぜ黒を引かないか」が RAW 側の実測を引いている |

**注意。** この装置が書かれた時点の一次報告は `docs/media-formats.md` で、これは
**main に無い**(branch `media-format-strategy` にだけある)。
[docs/README.md](../../../docs/README.md) の「未作成文書の例外」表に登録済みのパスで、
このディレクトリのソースコメントと下の表はそのまま `docs/media-formats.md §N` を引く。
節番号で照合したい場合は
`git show origin/media-format-strategy:docs/media-formats.md` を読むこと。

## 由来

| | |
|---|---|
| 測定日 | 2026-08-03 |
| 元ブランチ | `origin/media-format-strategy` |
| 元パス | `tools/bench_media/` |
| 取り込んだ commit | `160fc30b` (このディレクトリを最後に触った commit)。前段は `5c507a8d` (「vendor RAW is cheaper than EXR, and more honest than PNG」) と `106002c2` |
| 測定環境 | Windows 11 / MinGW (MSYS2)、laptop。**中央値と幅で読む前提**の数字 |

ファイル本体は branch の内容を**1バイトも変えずに**持ち込んである
(この README を除く)。したがって `bench_*.cpp` / `*/CMakeLists.txt` の
コメントに出てくる自己言及パスは旧 `tools/bench_media/...` のままである。
下の「使い方」が現行パスでの正しいコマンドで、置き換えは
`tools/bench_media/` → `tools/bench/media/` の1対1。

## 何を測るか

| | |
|---|---|
| `gen_exr.cpp` | 4K float RGB の EXR を NONE / ZIP / PIZ / ZIPS で書く。**内容はグラデーション + 固定パターン + 画素ごとのノイズ (決定的 LCG)** — 平坦な画像は圧縮が効きすぎてデコード時間の嘘になるため |
| `bench_exr.cpp` | 公式 OpenEXR で全画素を float に展開する時間。`FrameBuffer` で **R/G/B を別平面に**読む (= `loadExr` がやることと同じ。RGBA 詰め替えはしない) |
| `bench_oiio.cpp` | 同じことを OpenImageIO の `read_image(..., TypeDesc::FLOAT, ...)` で。**OIIO はインタリーブで返す**ので、両者は厳密には同じ仕事をしていない — `docs/media-formats.md` §3.2 の但し書きを読むこと |
| `raw/bench_raw.cpp` | LibRaw で `unpack()` の時間を測り、**同時に「これは測定値か」を印字する** — CFA パターン / ブラックレベル / ビット深度 / 生値の先頭、そして EXIF (露光・ISO・絞り・焦点距離・時刻)。`dcraw_process()` は**呼ばない** (デモザイクとトーンマップをする側なので)。さらに **`get_decoder_info()` を `unpack()` の前に呼び、コーデック名と `UNSUPPORTED_FORMAT` 旗、`NEFCompression`、`canon.Quality` を出す** — 非可逆かどうかを**1画素も復号せずに**判定できるため (`docs/media-formats.md` §4.8) |
| `stats.sh` | 上の `ms=` 行から中央値・最小・最大・幅を出す。**1回目(コールドディスク)を捨てる** |

どれも 1 行 1 回で `ms=` を出す。**中央値と幅で読むこと** — このプロジェクトの
規律として、1 回の数字は報告しない (`stats.sh` がそれをやる)。

## 再実行

必要なもの: CMake ≥ 3.20 / C++17 コンパイラ / Ninja / `git` / **ネットワーク**
(3つとも依存を FetchContent で取る: OpenEXR v3.4.13、OpenImageIO v3.1.16.0、
LibRaw 0.22.2)。`stats.sh` は POSIX `sh` + `awk`
(Windows では MSYS2 / Git Bash)。RAW の計測にはサンプル RAW ファイルが要る。

```sh
# OpenEXR 側 (2026-08-03 実測済み: 建つ)
cmake -S tools/bench/media/exr -B /short/path/exrb -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /short/path/exrb
/short/path/exrb/exrgen   bench_zip.exr zip 4096 2160
/short/path/exrb/exrbench bench_zip.exr 11 | sh tools/bench/media/stats.sh

# LibRaw 側 (2026-08-03 実測済み: 建つ)
cmake -S tools/bench/media/raw -B /short/path/rawb -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /short/path/rawb
/short/path/rawb/rawbench sample.CR3 9          # stdout=時間 / stderr=素性の表
/short/path/rawb/rawbench sample.CR3 9 2>/dev/null | sh tools/bench/media/stats.sh

# OIIO 側 (2026-08-03 時点で MinGW では建たなかった。これ自体が結論)
cmake -S tools/bench/media/oiio -B /short/path/oiiob -G Ninja -DCMAKE_BUILD_TYPE=Release
```

サンプル RAW は [raw.pixls.us](https://raw.pixls.us/) の CC0 データセットから。
**ダウンロードには `curl -L` が要る** (`/data/...` は `/download/data/...` へ 301)。

## 罠

- **パスを短くすること。** 最初の計測はビルドツリーが深すぎて
  `CMAKE_OBJECT_PATH_MAX` (250 文字) に当たり、OpenEXR の vendored deflate の
  サブビルドが落ちた。**OIIO のせいだと誤読しかけた。** ビルドツリーの根を
  50 文字程度に置けば通る。
- **LibRaw には CMakeLists.txt が無い** (上流が 2014 年に公式サポートをやめた)。
  `raw/CMakeLists.txt` はソースを直に並べる形で、`*_ph.cpp` の除外と
  Windows の `ws2_32` が**両方とも必須**。理由はそのファイルのコメントに書いてある。
- 4K float RGB の fixture は 1 枚 **85–106 MB**、実機 RAW は 1 枚 **10–29 MB**。
  gitignore 済みの場所に置くこと。**どちらもリポジトリに入れない**
  (RAW は権利の問題もある)。
- 2 回目以降はページキャッシュに乗る。`stats.sh` は **1 回目を捨てて**
  中央値を取る (= ディスクではなくデコードを測っている)。
- **依存のバージョンは 2026-08-03 に固定されている。** 上流が動けば同じ数字は
  出ない。数字を比べるときは tag を揃えること。
