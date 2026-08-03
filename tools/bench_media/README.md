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
| `bench_oiio.cpp` | 同じことを OpenImageIO の `read_image(..., TypeDesc::FLOAT, ...)` で。**OIIO はインタリーブで返す**ので、両者は厳密には同じ仕事をしていない — docs/media-formats.md §3.4 の但し書きを読むこと |

どちらも 1 行 1 回で `ms=` を出す。**中央値と幅で読むこと** — このプロジェクトの
規律として、1 回の数字は報告しない (`tools/bench_media/stats.sh` がそれをやる)。

## 使い方

```sh
# OpenEXR 側 (実測済み: 建つ)
cmake -S tools/bench_media/exr -B /short/path/exrb -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /short/path/exrb
/short/path/exrb/exrgen   bench_zip.exr zip 4096 2160
/short/path/exrb/exrbench bench_zip.exr 11 | sh tools/bench_media/stats.sh

# OIIO 側 (2026-08-03 時点で MinGW では建たなかった。docs/media-formats.md §3.3.1)
cmake -S tools/bench_media/oiio -B /short/path/oiiob -G Ninja -DCMAKE_BUILD_TYPE=Release
```

## 罠

- **パスを短くすること。** 最初の計測はビルドツリーが深すぎて
  `CMAKE_OBJECT_PATH_MAX` (250 文字) に当たり、OpenEXR の vendored deflate の
  サブビルドが落ちた。**OIIO のせいだと誤読しかけた。** ビルドツリーの根を
  50 文字程度に置けば通る。
- 4K float RGB の fixture は 1 枚 **85–106 MB** ある。3 通り作ると 290 MB。
  gitignore 済みの場所に置くこと。**リポジトリに入れない。**
- 2 回目以降はページキャッシュに乗る。`stats.sh` は **1 回目を捨てて**
  中央値を取る (= ディスクではなくデコードを測っている)。
