# bench_media — 形式リーダのビルドコストと読み出し速度を測るハーネス

**これは `viewer` の一部ではない。** ルートの
[CMakeLists.txt](../../CMakeLists.txt) はこのディレクトリを一切参照しておらず、
`viewer` / `viewer-serve` / plugins のどれにもリンクされない。selftest でもない。
**別プロジェクトとして自分で configure したときにだけ**建つ。

[docs/media-formats.md](../../docs/media-formats.md) の §3 の数字は、
これで測った。数字を疑う人が同じ手順を踏めるように置いてある。

## 何を測るか

| | |
|---|---|
| `gen_exr.cpp` | 4K float RGB の EXR を NONE / ZIP / PIZ / ZIPS で書く。**内容はグラデーション + 固定パターン + 画素ごとのノイズ (決定的 LCG)** — 平坦な画像は圧縮が効きすぎてデコード時間の嘘になるため |
| `bench_exr.cpp` | 公式 OpenEXR で全画素を float に展開する時間。`FrameBuffer` で **R/G/B を別平面に**読む (= `loadExr` がやることと同じ。RGBA 詰め替えはしない) |
| `bench_oiio.cpp` | 同じことを OpenImageIO の `read_image(..., TypeDesc::FLOAT, ...)` で。**OIIO はインタリーブで返す**ので、両者は厳密には同じ仕事をしていない — docs/media-formats.md §3.2 の但し書きを読むこと |
| `raw/bench_raw.cpp` | LibRaw で `unpack()` の時間を測り、**同時に「これは測定値か」を印字する** — CFA パターン / ブラックレベル / ビット深度 / 生値の先頭、そして EXIF (露光・ISO・絞り・焦点距離・時刻)。`dcraw_process()` は**呼ばない** (デモザイクとトーンマップをする側なので) |

どれも 1 行 1 回で `ms=` を出す。**中央値と幅で読むこと** — このプロジェクトの
規律として、1 回の数字は報告しない (`tools/bench_media/stats.sh` がそれをやる)。

## 使い方

```sh
# OpenEXR 側 (実測済み: 建つ)
cmake -S tools/bench_media/exr -B /short/path/exrb -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /short/path/exrb
/short/path/exrb/exrgen   bench_zip.exr zip 4096 2160
/short/path/exrb/exrbench bench_zip.exr 11 | sh tools/bench_media/stats.sh

# LibRaw 側 (実測済み: 建つ)
cmake -S tools/bench_media/raw -B /short/path/rawb -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /short/path/rawb
/short/path/rawb/rawbench sample.CR3 9          # stdout=時間 / stderr=素性の表
/short/path/rawb/rawbench sample.CR3 9 2>/dev/null | sh tools/bench_media/stats.sh

# OIIO 側 (2026-08-03 時点で MinGW では建たなかった。docs/media-formats.md §5)
cmake -S tools/bench_media/oiio -B /short/path/oiiob -G Ninja -DCMAKE_BUILD_TYPE=Release
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
  (RAW は権利の問題もある — docs/media-formats.md §11.1)。
- 2 回目以降はページキャッシュに乗る。`stats.sh` は **1 回目を捨てて**
  中央値を取る (= ディスクではなくデコードを測っている)。
