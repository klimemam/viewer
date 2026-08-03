# 読めない形式をどう読むか — 変換アダプタの設計

ユーザー提起 (2026-08-03):「そもそも npy も (F,H,W,C) と類似以外は読めないし、
色々なフォーマット対応を考えると提案の内容だけだと弱い。npz に x 軸を入れ込んだ
場合とかもあるし。何かドメイン特化な変換スクリプトを書くか、python のプログラムを
書いておいてそれをかますかしたい」。

## 1. なぜ C++ に形式を足し続けるのが負け戦か

今の loader は `decodeNpyBuffer` の形の**推測**で成り立っている:

- 3次元 `(3,H,W)` は「3ch の画像」か「3枚の stack」か **決められない** — 先頭が4以下なら
  チャンネル、という heuristic + `--npy-axis` の手動上書きで凌いでいる
- 4次元より深いものは**先頭を黙って取る**
- 1次元は「高さ1の画像」になる (docs/npz-design.md の欠陥)

形式が増えるたびにこの推測表が伸び、**推測が外れたときに黙って間違った絵を出す**。
測定器としては最悪の失敗の仕方で、しかも増え続ける。

**根本原因は「ファイルは自分が何であるかを言わない」こと。** 層 (frame ⊂ stack ⊂
series ⊂ batch)・軸・単位はドメインの知識であって、バイト列からは復元できない。

## 2. 決定的な事実 — その仕組みは既にこのリポジトリにある

リモートは **別プロセスに stdio で喋る**構造で動いている (`core/remote.cpp` /
`core/serve.cpp` / `remote_proto.h`、VERSION=5)。ローカルの Browse すら
`local://` で同じ経路を通る。つまり:

> **「外部プロセスが読んで、正典の形で返す」という配線は、実装済みで実運用中。**

アダプタはこの形をもう一度使うだけでよい。新しい機構ではない。

## 3. 提案 — importer は「層を宣言する」プロセス

### 3.1 契約 (viewer が知るべきこと)

    viewer-import <path> [--opt k=v ...]
      → stdout に manifest (JSON) + payload (npy バイト列)

manifest が**推測ではなく宣言**を運ぶ:

```json
{ "adapter": "acme_raw", "adapter_version": "1.2.0",
  "batch": "20260803_dark_sweep",
  "stacks": [
    { "name": "dark", "frames": 24, "w": 640, "h": 480, "ch": 1,
      "dtype": "u16", "cfa": "bayer", "cfa_pattern": "RGGB",
      "axis": { "name": "exposure", "unit": "ms",
                "values": [1.0, 2.0, 5.0, 10.0] },
      "payload": "stack0" }
  ],
  "metadata": { "gain_db": 6.0, "sensor": "IMX999", "temp_C": 25.3 } }
```

- **層は宣言される**。「これは 24 枚の stack だ」と adapter が言う。viewer は推測しない
- **軸も単位も宣言される**。npz に x 軸が入っている件はここで自然に解ける
- **CFA も宣言される**。今は doc の属性として後から人が設定している
- **metadata はそのまま provenance に乗る**

### 3.2 どこで動かすか — ローカルもリモートも同じ

peer は既に**向こう側のマシンで動くプロセス**。adapter も同じ場所で動かせる:

- ローカル: viewer が adapter を起動
- リモート: peer が adapter を起動し、結果だけが回線を流れる
  (生の巨大ファイルを転送しない — 今の設計思想と一致)

### 3.3 Python 参照実装を同梱する

`tools/import/` に:

- `viewer_import.py` — manifest を書くための薄いライブラリ
  (`emit_stack(array, name=..., axis=..., unit=...)` だけ)
- 例: `adapter_npz_keys.py` (key 付き npz)、`adapter_hdf5.py`、`adapter_tiff_stack.py`
- ユーザーは自分のドメイン用に**30行くらい**書けば済む

viewer 側の設定は「この拡張子はこの adapter」という表 (prefs)。
**自動探索はしない** — 任意のスクリプトが勝手に走るのは許さない。明示設定のみ。

## 4. 何が良くなるか

- **推測が減る。** `--npy-axis` のような「人が形を教える」旗は、adapter が宣言する世界では要らない
- **形式追加が C++ の変更でなくなる。** ビルドもリリースも要らない
- **provenance が正確になる。** どの adapter の何版が作った数値かが、
  エクスポートの出所行 (いまコミットハッシュを出している行) に並ぶ
- **Watch と噛み合う。** 監視対象は**元ファイル**、変化したら adapter を通して読み直す
  (docs/watch-design.md の再読込がそのまま使える)
- **1形式1ファイルの責務。** 壊れたときにどこを直すか自明

## 5. コスト・危険

- **信頼**: 外部プロセスを起動する。明示設定のみ、実行前に何を起動するか表示する
- **速度**: 起動 + シリアライズのオーバーヘッド。大きな stack はキャッシュが要る
  (変換結果を scratch に置き、元ファイルの mtime/size で無効化 — 参照化の
  identity tuple と同じ考え方)
- **Python 依存**: viewer 本体は今も C++ だけで動く。adapter を使う人だけが Python を要る
  (numpy は既に `tools/gen_testdata.py` が要求している)
- **部分実装の誘惑**: manifest に嘘を書く adapter は viewer から検証できない。
  最低限、宣言された shape と payload の実バイト数の一致は viewer が検査する

## 6. native のままにするもの

**`.npy` / `.npz` / raw は今後も native**。日常の 9 割で、外部プロセスを挟む理由がない。
docs/npz-design.md の修正 (1次元を画像にしない・picker・軸候補) は**そのまま進める** —
アダプタは「その先の形式」のための逃げ道であって、npz を置き換えるものではない。

## 7. 段階

| # | 内容 | 検証 |
|---|---|---|
| 1 | manifest の形を確定 (この文書 §3.1 を仕様化) | 例 JSON を2つ書いて層モデルに矛盾がないこと |
| 2 | `tools/import/viewer_import.py` + adapter 1本 (key 付き npz) | Python 側だけで完結、手で叩ける |
| 3 | viewer 側: 拡張子→adapter の表 (prefs) と起動・manifest 解釈 | 新 selftest: 偽 adapter (echo するだけの実行ファイル) を起動し、宣言どおり層ができること |
| 4 | payload 検証 (宣言 shape と実バイト数) + 失敗時の言い方 | 嘘 manifest を食わせて拒否されること |
| 5 | キャッシュ (mtime/size で無効化) | 2回目が adapter を起動しないこと |
| 6 | peer 側での実行 (リモート) | 既存の remote selftest に1本足す |

## 8. 決めてほしいこと

1. **この方向で進めるか** (推奨: 進める。§6 のとおり npz 修正は独立で先行)
2. **manifest は JSON でよいか**、それとも既存 remote_proto の枠に載せるか
   (推奨: JSON。人が手で書けることが adapter を書く敷居を決める)
3. **adapter の起動は明示設定のみでよいか** (推奨: よい。自動探索はしない)
4. **v1 の適用範囲**: ローカルのみか、最初からリモートもか
   (推奨: ローカル先行。peer 側は §7-6 で追う)
