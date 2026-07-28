# 起動手順

3通りあります。**どの構成でもウィンドウは手元のマシンに出ます**(ローカル・ネイティブ実行がボトムライン仕様)。

| 構成 | ウィンドウ | 画素の場所 | 準備 |
|---|---|---|---|
| ローカル | 手元 | 手元 | なし |
| **リモート(推奨)** | **手元** | サーバ | **不要**(初回接続時にサーバが自動導入) |
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

**毎回コマンドラインを開くのが面倒なら、次節でショートカットを作ってください**
(`win64\install_shortcut.cmd` をダブルクリックするだけ)。

## 0-2. デスクトップ / スタートメニューから起動する

```bat
win64\install_shortcut.cmd                                    :: Windows
```
```bash
./linux-x64/install_shortcut.sh                               # Linux / macOS
```

デスクトップとスタートメニュー(Linux はアプリ一覧、macOS は `~/Applications`)に
**exe をその場で指す**ショートカットができます。**コピーはしません** —— だから
`update.cmd` でバイナリを更新してもショートカットはそのまま最新版を起動します。
消すときは `-Uninstall` / `--uninstall`。

| やりたいこと | やり方 |
|---|---|
| タスクバーに常駐 | 一度起動 → タスクバーのボタンを右クリック → **ピン留め** |
| ファイルを開く | `.npy` / `.raw` を**ショートカットにドロップ**(起動後のウィンドウへの D&D も可) |
| **サーバ直結のショートカット** | `install_shortcut.cmd -RemoteHost user@server -RemotePath /data/run42`<br>(Linux/macOS は `--host user@server --path /data/run42`)。起動と同時に接続し、Remote パネルにそのフォルダが出ます |

パスを省くとホーム(`~`)を開きます。`.npy` を指定すればその画像を直接開きます。

**アイコン**: **CFA の 2x2(R/Gr/Gb/B)を枠で囲んだ印**で、枠の色が状態を表します ——
**青 = ローカル、緑 = リモート**(ステータスバーのリンク表示と同じ緑)。

| どこ | 見え方 |
|---|---|
| デスクトップ/スタートメニューの `viewer` | 青 |
| デスクトップ/スタートメニューの `viewer (host)` | **緑**(サーバ直結のショートカットだと一目で分かる) |
| 走っているウィンドウ・タスクバーのボタン | **接続したら青→緑に変わる**(切断で戻る) |
| タイトルバー | `frame_000.npy - viewer [user@server]` |

アイコンは実行時に描いているので、接続/切断した瞬間にタスクバーの色が変わります。
ショートカット用の緑アイコンはビルド時に `viewer-remote.ico` / `viewer-remote.png` として
生成され、バイナリの隣に置かれます(無ければ exe のアイコンにフォールバック)。

<details><summary>なぜコンソール窓が一瞬出る(ことがある)のか</summary>

`viewer.exe` は**コンソールアプリのまま**にしてあります。GUI サブシステムにすると
`viewer --help` や `--bench`、`--remote-selftest` を打ったときに cmd が終了を待たず、
出力も終了コードも取れなくなるためです(VSCode タスクが両方を読んでいます)。

代わりに、**自分専用のコンソール(= 誰も繋がっていないコンソール)なら起動直後に閉じる**
ようにしてあります。シェルから起動したときは何もしません。加えてインストーラは
ショートカットを「最小化で起動」に設定するので、閉じるまでの一瞬すら見えません。
手で作ったショートカットで黒窓が一瞬光る場合は、プロパティの**実行時の大きさ = 最小化**を
選んでください。
</details>

## 1. ローカル(ビルド環境のある開発機)

```bash
cmake -S . -B build-mingw -G Ninja -DCMAKE_BUILD_TYPE=Release   # 初回のみ
cmake --build build-mingw
./build-mingw/viewer                       # Windows: .\build-mingw\viewer.exe
./build-mingw/viewer path/to/image.npy     # ファイル指定で開く
```

ソースからビルドした場合もショートカットは同じスクリプトで作れます
(`tools\install_shortcut.cmd` / `tools/install_shortcut.sh`)。ビルド時に
`build*/icons/` へアイコン(`viewer.ico` / `viewer.png`)が生成され、スクリプトはそれを拾います。

## 2. リモート(サーバのデータを手元から見る)

### 2-1. 準備

**サーバ側の準備は不要です。** 初回接続時に、サーバが自分で `binaries` ブランチから
`viewer-serve` とプラグインを取得し `~/.viewer/` に配置します(サーバに git と
ネットワークが必要)。手元のバイナリを送りつけるわけではないので、Windows 版の
配布物が重くなることもありません。

必要なのは**公開鍵認証が通ること**だけです:

```powershell
ssh -o BatchMode=yes user@server true; echo $?      # 0 なら準備完了
```

パスワードを聞かれる場合は鍵を作って登録してください:

```powershell
ssh-keygen -t ed25519
type $env:USERPROFILE\.ssh\id_ed25519.pub | ssh user@server "cat >> ~/.ssh/authorized_keys"
```

### 2-2. 接続してから選ぶ

1. **File > Start Remote (ssh)...**
2. **ホストだけ**入力(`user@server`、`~/.ssh/config` の Host エイリアスも可)
3. Connect → 初回は `installing the viewer peer on ...` と出て自動導入
4. **Browse パネルにサーバの一覧が現れます**(ローカルのフォルダも同じパネルで見られます:
   **File > Browse Folder (Local)...**)。

| したいこと | 操作 |
|---|---|
| 中身を見る(開かない) | ファイル行を**シングルクリック** → 使い捨てプレビュー。Files には登録されず、次のクリックで置き換わる |
| 連番を送って見る | 連番は 1 行(`frame_???.npy [24 frames]`)にまとまる。クリックでプレビュー、パネル下端の `< >` / スライダー / `,` `.` で**開かずにフレーム送り** |
| 正式に開く | **ダブルクリック**(または測定・比較に使った瞬間に自動で昇格) |
| 任意の組み合わせ | Ctrl/Shift+クリックで選んで「Open N selected as stack」 |
| フォルダ丸ごと | 右クリック「Open folder (all stacks below)」 |
| 開かずに時間統計 | 連番行の右クリック「Temporal stats (server)」 |

フォルダを開くと **Select sequences** ダイアログが必ず出ます。ここで
`include`/`exclude` にライブで絞り込みを打ち込み(打鍵ごとに反映)、
**「N 個の別スタック」か「まとめて 1 スタック」**か、
**「1 batch」か「トップフォルダ毎に batch」**かを決めてから読み込みます。
リモートは 1 フレーム = 1 転送なので、読む前に絞るのが効きます。

パスの形(絶対か `~` 相対か)を先に考える必要はありません。接続してから見て選ぶだけです。

**URL を直接渡したい場合**(CLI やペースト)は以下すべて有効です:

| 記法 | 意味 |
|---|---|
| `ssh://user@host/data/run42` | 絶対パス(RFC 3986) |
| `ssh://user@host:2222/data` | **ポート指定** |
| `ssh://user@host/~/data` | ホーム相対(git 拡張) |
| `user@host:~/data` | scp 形式 |

```powershell
.\viewer.exe ssh://user@server/data/run42/frame_000.npy
```

ピアの場所を変えたい場合のみ、ダイアログの **advanced** で指定します
(既定 `~/.viewer/viewer-serve`)。**File > Update remote peer** で更新できます。
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
| `install: desktop / Start menu shortcut` | **いまビルドした exe を指すショートカットを作る** |

ホスト名などは毎回聞かれます。固定したい場合は `tasks.json` の `inputs` の `default` を書き換えてください。

### 注意: VSCode の Remote-SSH を使っている場合

**Remote-SSH で開いた VSCode のターミナルは「サーバ側」です。** そこから viewer を起動すると、GUI もサーバ側で立ち上がってしまい(あるいは DISPLAY が無くて起動せず)、いま解決しようとしている問題そのものに戻ります。

**viewer は必ず手元(ローカル)のウィンドウ/ターミナルから起動してください。** ソースの編集を Remote-SSH で行い、viewer の起動だけローカルの VSCode ウィンドウで行う、という使い分けになります。同梱のタスクはローカルウィンドウで実行される前提で書いてあります。

---

## 用語(詳しくは docs/terminology.md)

| 層 | 何か |
|---|---|
| **frame** | 1 枚 |
| **stack** | 同一条件の連番。時間軸を持つので σ_t / FPN が意味を持つ |
| **series (系列)** | 条件を 1 つ振った並び。パラメータ軸を持つのでリニアリティ等のフィットが意味を持つ(実装中) |
| **batch (塊)** | 開いたものの入れ物。1 回の Open = 1 batch が既定。**名前を付け替えられます**(Files の見出しを右クリック) |

stack も batch も右クリック(stack は F2 でも)でリネームでき、名前はセッションに残ります。
`Move to batch` で入れ替えもできます。同名ファイルだらけの測定でも、人間に分かる名前を付けて
区別するための層です。

## A/B 比較

- B の指定は **Files で対象行を右クリック > Set as compare B**(名前ではなく実体を指すので、
  同名スタックだらけでも取り違えません)。選ばれている行には青い **B** が付きます
- 両方 stack なら **A を送ると B も同じフレーム番号に追従**します(`Compare A/B > B follows A's frame number`)
- **表示レンジの関係**は 3 通り(Inspector か Compare メニュー):
  `each keeps its own` / `B uses A's range` / **`auto over both (union)`**。
  露出が違うもの同士は union にすると**どちらも飽和せず**に比べられます

## 現在の制限(実装中)

- リモートで開けるのは **`.npy` のみ**(C order / Fortran order 両対応)。RAW はレシピの受け渡しが未実装、`.npz` も未対応
- 自動導入はサーバに **git とネットワーク**がある前提。無い場合は `~/.viewer/viewer-serve` に手で置けば動きます
- **`MEASURE` は実装済み** — 塊を開くと転送を待たずサーバ側で時間統計を計算し、`[server, N frames]` タグ付きで表示します(File > Sequence loading > Remote processing で切替)
- 先読み・タイルキャッシュ未実装のため、ズーム/パンのたびに取得が発生します
- **series 層は実装中**。今のリニアリティは開いている stack を全部 1 本の掃引として扱うので、
  掃引を 2 つ同時に開くと混ざります
