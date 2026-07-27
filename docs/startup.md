# 起動手順

3通りあります。**どの構成でもウィンドウは手元のマシンに出ます**(ローカル・ネイティブ実行がボトムライン仕様)。

| 構成 | ウィンドウ | 画素の場所 | 準備 |
|---|---|---|---|
| ローカル | 手元 | 手元 | なし |
| **リモート(推奨)** | **手元** | サーバ | サーバに `viewer-serve` を1つ置く |
| ローカル(テスト用ピア) | 手元 | 手元(別プロセス経由) | なし |

---

## 0. ビルド環境の無い Windows PC で使う(推奨: 作業用 PC はこれ)

main に push されるたびに、CI がビルド済みバイナリを **`binaries` ブランチ**へ発行します。
作業用 PC に必要なのは git だけです:

```bat
git clone -b binaries --single-branch <このリポジトリのURL> viewer-bin
cd viewer-bin
win64\viewer.exe
```

**更新は `update.cmd` を実行するだけ**(Linux/macOS は `./update.sh`)。
素の `git pull` は使えません — このブランチは古い exe をリポジトリに溜めないよう
毎回履歴を置き換えるためで、正しい取得手順をスクリプトにしてあります。

中身: `win64/viewer.exe`(GUI)+ `win64/plugins/`、`linux-x64/viewer-serve`(サーバに置く方)ほか。

## 1. ローカル(ビルド環境のある開発機)

```bash
cmake -S . -B build-mingw -G Ninja -DCMAKE_BUILD_TYPE=Release   # 初回のみ
cmake --build build-mingw
./build-mingw/viewer                       # Windows: .\build-mingw\viewer.exe
./build-mingw/viewer path/to/image.npy     # ファイル指定で開く
```

## 2. リモート(サーバのデータを手元から見る)

### 2-1. サーバ側にバイナリを置く(初回だけ)

**手元とサーバで OS が違う場合、バイナリは2つ必要です**(例: 手元 Windows / サーバ Linux)。

| 置く場所 | 使うバイナリ | 役割 |
|---|---|---|
| 手元(Windows) | **`viewer.exe`**(Windows 版) | GUI。ウィンドウを出す側 |
| サーバ(Linux) | **`viewer-serve`**(Linux 版) | 画素を返す側。ウィンドウは出さない |

**サーバには `viewer` ではなく `viewer-serve` を置いてください。** GUI 版は OpenGL と X11 にリンクしているため、それらが入っていない計算機では**起動すらできません**(`--serve` に到達する前に動的リンカが失敗します)。`viewer-serve` は miniz と C++ ランタイム以外に何もリンクしていない 0.3 MB の実行ファイルで、素のサーバでそのまま動きます。

```bash
# サーバに開発環境があるなら、サーバ上でビルドするのが一番簡単です。
# viewer-serve ターゲットは GUI 依存(GLFW/OpenGL/X11)を一切持ちません:
ssh user@host 'git clone <repo> viewer && cmake -S viewer -B viewer/b -DCMAKE_BUILD_TYPE=Release && cmake --build viewer/b --target viewer-serve'
ssh user@host 'install -D viewer/b/viewer-serve ~/bin/viewer-serve'

# ビルドしない場合は binaries ブランチの linux-x64/viewer-serve を scp でも可
ssh user@host '~/bin/viewer-serve --help'           # 動くことの確認
```

プロトコルは全メッセージが 32bit 整数の並びなので、**Windows ↔ Linux でそのまま通ります**(x86-64 はどちらもリトルエンディアン、構造体にパディングも入りません)。

### 2-2. 手元から開く

GUI からは **File > Open Remote (ssh://)...** で URL とリモート側バイナリのパスを
入力できます(どちらも記憶され、次回はプリフィルされます)。CLI なら:

```bash
.\build-mingw\viewer.exe ssh://user@host/data/run42/frame_000.npy --remote-exe ~/bin/viewer-serve
```

これだけです。内部で次が起きます:

```
手元の viewer.exe ──ssh user@host ~/bin/viewer-serve──> サーバ側のピア
              <── 見えている領域の画素 / 測定した数値 だけ ──
```

- **`~/.ssh/config` にエイリアス**を書いておくと短くなります(`Host lab1` → `ssh://lab1/data/...`)
- 認証は ssh に任せています。パスワード対話はできないので、**公開鍵認証を設定してください**
  (`-o BatchMode=yes` で起動するため、鍵がないと即座に失敗します)
- `--remote-exe` は省略すると `viewer` を探します。`viewer-serve` を置く運用では常に指定してください

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
