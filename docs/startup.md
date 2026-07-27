# 起動手順

3通りあります。**どの構成でもウィンドウは手元のマシンに出ます**(ローカル・ネイティブ実行がボトムライン仕様)。

| 構成 | ウィンドウ | 画素の場所 | 準備 |
|---|---|---|---|
| ローカル | 手元 | 手元 | なし |
| **リモート(推奨)** | **手元** | サーバ | サーバに同じバイナリを1つ置く |
| ローカル(テスト用ピア) | 手元 | 手元(別プロセス経由) | なし |

---

## 1. ローカル

```bash
cmake -S . -B build-mingw -G Ninja -DCMAKE_BUILD_TYPE=Release   # 初回のみ
cmake --build build-mingw
./build-mingw/viewer                       # Windows: .\build-mingw\viewer.exe
./build-mingw/viewer path/to/image.npy     # ファイル指定で開く
```

## 2. リモート(サーバのデータを手元から見る)

### 2-1. サーバ側にバイナリを置く(初回だけ)

サーバ用にビルドしたバイナリを1つ置くだけです。**デーモンは起動しません。ポートも開けません。**

```bash
scp build-mingw/viewer user@host:~/bin/viewer
ssh user@host '~/bin/viewer --help | head -3'      # 動くことの確認
```

> サーバが Linux なら Linux 用にビルドしたものを置いてください。
> 依存は自己完結しているので、置くのは実行ファイル1つで済みます。

### 2-2. 手元から開く

```bash
./build-mingw/viewer ssh://user@host/data/run42/frame_000.npy --remote-exe ~/bin/viewer
```

これだけです。内部で次が起きます:

```
手元の viewer ──ssh user@host ~/bin/viewer --serve──> サーバ側のピア
              <── 見えている領域の画素 / 測定した数値 だけ ──
```

- **`~/.ssh/config` にエイリアス**を書いておくと短くなります(`Host lab1` → `ssh://lab1/data/...`)
- 認証は ssh に任せています。パスワード対話はできないので、**公開鍵認証を設定してください**
  (`-o BatchMode=yes` で起動するため、鍵がないと即座に失敗します)
- リモート側のバイナリが PATH にあるなら `--remote-exe` は省略できます

### 2-3. 何が速くなるのか

`ssh -X` はウィンドウ全体(1600x1000 で約 6.4 MB)を**毎フレーム**送るため、実測で 500 ms/frame・入力遅延 300 ms でした。この方式で送るのは:

| 操作 | 転送量 |
|---|---|
| 文字入力・ROI 操作・メニュー・表示レンジ変更 | **0 バイト** |
| 視野を変えた(ズーム/パン/フレーム送り) | 見えている領域だけ(12 Mpx f32 のフィット表示で 4.6 MB、これは圧縮最悪のノイズ画像。構造のあるデータなら1桁以上小さい) |
| 測定(実装予定) | 数値のみ(数十バイト) |

## 3. テスト用ピア(サーバなしでリモート経路を試す)

```bash
./build-mingw/viewer "local://tools/testdata/grad_u16.npy"   # ssh を挟まず同じ経路
./build-mingw/viewer --remote-selftest tools/testdata/grad_u16.npy
```

`--remote-selftest` はピアを起動して、返ってきた**全サンプルをローカルのデコード結果と突き合わせ**ます。ズレがあれば FAIL と件数が出ます。

---

## VSCode から

`.vscode/tasks.json` を同梱しています。**Ctrl+Shift+B** でビルド、**Ctrl+Shift+P → Tasks: Run Task** で次が選べます:

| タスク | 内容 |
|---|---|
| `build` / `configure` | ビルド(既定タスク)/ 初回の生成 |
| `run` | ローカルで開く |
| **`run: open remote (ssh)`** | **ホスト・パス・リモート側バイナリを聞いてから開く** |
| `remote: deploy this build to the host` | サーバへ `scp` して動作確認まで |
| `test: remote protocol self-check` | 上記のサンプル一致検証 |
| `test: frame-time benchmark` | フレーム時間の計測 |

ホスト名などは毎回聞かれます。固定したい場合は `tasks.json` の `inputs` の `default` を書き換えてください。

### 注意: VSCode の Remote-SSH を使っている場合

**Remote-SSH で開いた VSCode のターミナルは「サーバ側」です。** そこから viewer を起動すると、GUI もサーバ側で立ち上がってしまい(あるいは DISPLAY が無くて起動せず)、いま解決しようとしている問題そのものに戻ります。

**viewer は必ず手元(ローカル)のウィンドウ/ターミナルから起動してください。** ソースの編集を Remote-SSH で行い、viewer の起動だけローカルの VSCode ウィンドウで行う、という使い分けになります。同梱のタスクはローカルウィンドウで実行される前提で書いてあります。

---

## 現在の制限(実装中)

- リモートで開けるのは **`.npy` のみ**。RAW はレシピの受け渡しが未実装、`.npz` も未対応
- **フォルダ/連番のリモート読み込みは未配線**(`LIST` はプロトコルにあるが UI 未接続)
- **`MEASURE`(サーバ側で解析を実行)は未実装** — 現状、解析は手元に来た画素に対して走ります
- 先読み・タイルキャッシュ未実装のため、ズーム/パンのたびに取得が発生します
- Fortran order の `.npy` はサーバ側で拒否されます(ローカルで開いてください)
