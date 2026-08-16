# リモート実行リーダ — 「reader は peer 側で走る」の設計 (G11 / issue #180 裁定 B)

**状態: 設計 (2026-08-17, Fable)。** issue #180 のユーザー裁定 B —
`docs/input-adapters.md` §4.13.1 (2026-08-03)「**adapter は peer 側で走る**」を
そのとおり実装する — を受けた設計。対象は verify-matrix **G11**
(「決定はあるが実装が無く、拒否文もそれを言わない」、`docs/verify-matrix.md:640`)。

読んだもの: `docs/input-adapters.md` §4.12/§4.12.1/§4.13〜§4.13.2 /
`core/remote_proto.h` (protocol 11 全注釈) / `core/serve.cpp` /
`core/app/open_dispatch.inc` (`openRemote` :1819〜) / `core/app/session.inc`
(`openWithReader` :1514〜, `readerFinish` :1631〜) / `core/adapter.h` /
`core/app/loader_npz.inc` (`loadViewerStream` :1289, `vnzBuild`/`VnzFetch` :1212) /
`tools/import/run_adapter.py` / `docs/adapter-transport-review.md` (#44/#45 決着) /
`core/remote.cpp` (:332〜 peer の起動) / `core/selftest/rwatch.inc` (local:// peer 実測の型)。

---

## 0. 結論 — 決定6件を先に

| # | 問い | 決定 |
|---|---|---|
| 1 | 許可の形 | **peer プロセスの起動引数 `--serve-readers`(既定 閉)**。環境変数でも同梱ディレクトリでもない。#179 裁定 C はリンクを越えても成立する: client 側は memo/picker だけが起動でき (変更なし)、peer 側の同意は「その旗を立ててプロセスを起動した者」に付く。ssh では起動者 = client のユーザー自身 = 既に shell 権限を持つ者なので、旗は ssh に対しては新権限を与えない。旗が守るのは **ssh でない起動者** (将来の WebSocket front、serve.cpp :641 の注釈が名指しする転送) である |
| 2 | adapter ファイルの所在 | **クライアントから運ぶ** (reader 1 ファイル + `viewer_import.py` + `run_adapter.py` の 3 テキスト)。peer 側常設 (名前で解決) は採らない — §4.13.1 自身の文言・#148 の「同名別バイトの静かな分岐」・配備コスト・provenance の 4 点で負ける (§3) |
| 3 | プロトコル | **VERSION 12、FRAMING の版上げ** (7/8/11 と同種)。新 op `MSG_READER_RUN = 8`。META/TILE/MEASURE は**末尾追記の reader トレーラ** (`[str key][u32 node]`、MEASURE は `MRF_READER` ビットで宣言)。client は **送る前に** HELLO の数で断る (`readerTooOldText`) — v11 peer はトレーラを黙って読まないので、#124 型の「正しいラベルの下の違う画素」が起きうるため (§4.2) |
| 4 | MSG_TILE の原則 | **そのまま成り立つ**。reader が peer で materialise した画素の「source dtype」は **reader が宣言した dtype** (harness の 9 種、`b1` は serve.cpp :267 の既存表どおり `DT_U8`)。ファイルバイトも変換結果全体も回線を渡らない — 渡るのはヘッダ (テキスト) と間引きタイルと測定結果だけ (§4.3) |
| 5 | #44 との接続 | peer 上で `run_adapter.py --stream → ファイル` (凍結済みの枠付き v1、0.268 s/151 MB)。**キャッシュファイルがそのまま配布面**: `.vstream` の blob は C-order 連続なので、`openRaw` (serve.cpp :568) と同じ「offset + stride の ServedFile」で `readNpyRegion` が無改造で読む。キャッシュ鍵の mtime/size は **peer の stat** (§5) |
| 6 | 失敗の報告 | RUN 応答は `adapter::Run` が分けている 4 事実 (started / timedOut / exit / stderr) を**そのまま**運ぶ。文面は `readerFinish` (session.inc :1631) の文を関数に括って両経路で共有し、**機械名を足す** — 「どの python が無いのか」は machine を言わなければ直せない (§6) |

段階は 0〜5 (§8)。**stage 0 (拒否文が事実を言う) は設計から独立した Opus 仕事**で、
即日切り出せる。

---

## 1. 何が欠けているか — G11 の現状

- `docs/input-adapters.md` §4.13.1 (:1241) は 2026-08-03 に決めている:
  「データが向こうにあるのに adapter を手元で走らせると、生ファイルを転送してから
  変換することになる … **したがって adapter は peer 側で走る**」。
- `core/serve.cpp` にその口は無い。あるのは `MOP_PLUGIN_ANALYZE` (ABI v3 の
  解析プラグイン、serve.cpp :2491) で別物。
- `openRemote` (open_dispatch.inc :1847) は `peerServesName` が false のとき
  reader を提案せず断り、拒否文 (`imagefile::peerRefusal` の fall-through、
  imagefile.cpp :485) は「the peer serves .npy, PNG, …」— **決定の存在を
  言わない**。判断された拒否でも開いた道でもない、が G11 の全文である。

前提に置く実装済みの部品 (新発明ではないことの確認):

| 部品 | どこに既にあるか |
|---|---|
| 外部プロセス実行 (期限・kill・出力ファイル) | `core/adapter.cpp` `run()` — **意図的に viewer 状態フリー** (adapter.h 冒頭)。peer バイナリにそのままリンクできる |
| Python 探索 (numpy import が probe) | `adapter::findPython` |
| 運搬形式 | 枠付きストリーム v1/v2 — #44 レビューで**凍結済み** (`adapter-transport-review.md` §1.2)。`_esc`/LE 固定 (A2/A4) 込み |
| 木の検査と構築 | `vnzCheckSets`/`vnzCheckTree` (A3)・`vnzBuild` + **`VnzFetch` 継ぎ目** (loader_npz.inc :1212 — 「木の構築は 1 つ、fetch だけ差し替え」) |
| キャッシュ | `.vstream` を鍵付きでディスクに置く形 (session.inc :1552〜) — そのまま peer 側に写る |
| 版の規律 | remote_proto.h の「送る前に数字で断る」3 前例 (`npyReReadTooOldText` :736 / `pictureTooOldText` :750 / `rawTooOldText` :767) |

---

## 2. 許可の形 — 門は「起動した者」に付く

### 2.1 決定

`viewer --serve` / `viewer-serve` に **起動引数 `--serve-readers`** を足す。

- **既定は閉**。閉じたまま `MSG_READER_RUN` を受けたら、版でも typo でもない
  固有の一文で断る:
  `"this peer was started without --serve-readers: whoever starts viewer-serve
  decides whether readers sent by a client may run here"`。
- client (v12) は ssh / local:// の peer を起動するとき**常にこの旗を付ける**
  (`core/remote.cpp` :342/:352 の argv に 1 語)。
- `VIEWER_SERVE_PROTOCOL` (serve.cpp :669) で v11 を演じる peer は op 自体を
  `unknown request` で断る — 「旗が閉じている v12」と「op を知らない v11」が
  試験で区別できること。

### 2.2 なぜこの形か — #179 裁定 C がリンクを越えても成り立つ読み方

裁定 C の原則は「**文書や名前はコードを走らせない。走らせるのはユーザーの
明示選択だけ**」。リンクを越えると「ユーザー」が 2 人に割れる。それぞれに
同意の置き場所を与える:

1. **どの reader を走らせるか** — client のユーザーの選択のまま。起動できるのは
   従来どおり picker と memo だけ (§4.13「明示的に選んだものだけが走る」)。
   session の `readerhint` は名指しするだけで走らせない (§4.12.1) — この規律は
   remote でも 1 文字も動かない。**client の memo は「このマシンの選択」**であり、
   それが peer で意味を持つのは次の同意と交差したときだけ。
2. **この機械で他人のコードが走ってよいか** — peer 側の同意。その実体は
   「`--serve-readers` を付けてプロセスを起動した」という行為である。
   **peer は memo も session も hint も一切読まない** — peer にコードを
   走らせうる唯一のものは、v12 client が明示に送った RUN 要求 × 開いた旗、
   の積である。文書がコードを走らせる経路は両側に存在しない。

**ssh ではこの旗は新しい権限を与えない**ことを正直に書いておく: この転送の
認証モデルは「ssh already did the authentication」(remote_proto.h 冒頭) で、
`viewer-serve` を起動できる者は同じ手で `python` を起動できる。旗を立てるのが
client なのは矛盾ではなく、**既に持っている権限の行使**である。旗が実質を持つのは
handler が意図的に転送非依存 (serve.cpp :641「same requests, same replies」) で
ある点にある — WebSocket front が来た日、argv は**運用者**のもので client は
触れない。そのとき何もしなくても既定で閉じているのが、この設計の防御線である。

### 2.3 なぜ他の形でないか

- **VIEWER_SERVE_PLUGINS 型 (同梱ディレクトリ既定 ON) ではない**: plugin の同意は
  「機械の持ち主が dll をその場所に置いた」という**ファイルの存在**が担う。運ばれて
  くる reader には peer 側の同意の器がファイルとして存在しないので、同意は起動
  行為に付けるしかない。
- **環境変数ではない**: ssh は任意の環境変数を運ばない (AcceptEnv 制限)。argv は
  client が既に組んでいる (remote.cpp :342-352) ので、旗は追加配線ゼロで届く。
  peer 側の**補助設定** (interpreter の指定・キャッシュ置き場) は環境変数でよい
  (§5.3) — あれは同意ではなく設定だから。

**再訪条件**: 認証はあるが shell 同等でない転送 (WebSocket) が実装されたら、
旗 1 本で足りるか (接続ごとの許可・reader の allowlist が要るか) を再設計する。
それまでは無い転送のための機構を作らない。

---

## 3. adapter ファイルの所在 — クライアントから運ぶ

### 3.1 決定

`MSG_READER_RUN` が **3 つのテキストを運ぶ**: ユーザーの reader `.py` 1 ファイル、
`viewer_import.py`、`run_adapter.py`。peer は一時ディレクトリに書き、
`python -u run_adapter.py <file>:<func> <peerPath> --stream` を走らせ
(ローカルの argv、session.inc :1588 と同一形)、終わったら一時ディレクトリを消す。
残るのはキャッシュの `.vstream` だけ。

### 3.2 なぜ peer 側常設 (名前で解決) ではないか

1. **§4.13.1 自身がこの形を書いている**: 「adapter は**テキストなので protocol で
   送れる**」「**選択はローカルで行われたまま。peer が勝手に adapter を探すことは
   ない**」。peer 側の置き場を名前で引く形は、後半の文とまっすぐ衝突する。
2. **#148 の規則**: 名前解決を peer に置くと「client の `acme.py` と peer の
   `acme.py` が別バイト」が静かに成立し、同じファイルが端によって**別のコードで**
   読まれる。運べば、走ったバイト = memo が名指すバイト、が**構造で**一致する。
   abi-v3 §10 が C プラグインで払った parity 照合 (name+version の等値ゲート) が、
   Python では**源泉そのものを送れる**ため、丸ごと不要になる。
3. **配備**: peer は「~/bin にコピーされた 1 ファイル」(serve.cpp :1437 の注釈)。
   全計算ノードへ reader 木を同期させる形は、#10 が潰した「同じ名前の違う中身」を
   Python で再演する。
4. **provenance**: `adapter-transport-review.md` §5.1 の「申告で守る」(reader の
   内容ハッシュ + python/numpy 版) が、運搬型では**構築時に自動で真**になる。

### 3.3 代償 — 隠さず書く

- **兄弟 import は v1 では動かない** (`util.py` を import する reader)。運ぶのは
  1 ファイルだけなので、peer 上では Python 自身の `ImportError` になり、その
  traceback が**丸ごと** client の Reader パネルに出る (§6)。§4.9 の「メッセージが
  そのまま出る」がここでも成立するので、失敗は読める。
  **再訪条件**: 実運用の reader が複数ファイルを要求した時点で、「運ぶファイル
  集合を reader 側が宣言する」形 (manifest 1 行) を足す。
- **サイズ上限**: 運ぶ 3 ファイル合計 4 MB (reader はテキストである。超えたら
  名指しで断る)。メッセージ全体は既存の 64 MB 枠 (serve.cpp :2623) に収まる。
- **peer 上の実行権限**: 運ばれた reader は peer アカウントの権限で走る。これは
  仕様であって漏れではない — §2 の門がその同意である。

---

## 4. プロトコル — VERSION 12

### 4.1 版の性格と両向きの拒否

**FRAMING の版上げ** (7 = MOP_PLUGIN_ANALYZE、8 = MOP_SET_FOLD、11 = RawWire と
同種): v11 peer が既に返すどの答えの意味も変わらない。番号が要る理由も同じ 3 点 —

- **古い peer / 新しい client**: client は**送る前に** HELLO の数から断る。
  remote_proto.h に 4 本目の定型を足す:

  ```
  readerTooOldText(peerVersion, name):
    "<name>: a reader cannot run on this peer - it speaks protocol <N>, and
    readers over the link need 12 (update viewer-serve). Nothing ran:
    the file stays where it is; copy it here to use a reader today."
  ```

  送ってから学ぶ形にしない理由は #124 の再演防止である: v11 peer は META/TILE の
  **末尾トレーラを黙って読まない** (getRecipe :1278 と同じ「残りバイトが無ければ
  無かった」解釈)。origin が peer の配れる形式 (reader で読み直したい `.npy` 等)
  だった場合、**native の読みが成功として返り、client は reader のラベルを
  貼ってしまう** — 「正しいラベルの下の違う画素」は空欄より悪い、が protocol 9 の
  結論だった。
- **新しい peer / 古い client**: v11 client は reader op もトレーラも送らないので
  何も変わらない。一覧にも変化が無い (reader 対象のファイルは今日も淡色で
  listed — #111 の「見せて理由を言う」のまま) ので、`isScannableSuffix` :663 型の
  client-version ゲートは**不要**。これが FRAMING と断言できる根拠である。
- **接続時の自己更新**: 番号が、入っている peer を更新させる (毎版の理由)。

「旗が閉じた v12」の拒否 (§2.1) は too-old と**別の文**である — 版は送る前に
client が書き、旗は peer が書く。3 状態 (古い / 閉じている / 開いている) が
すべて別の文で読める。

### 4.2 ワイヤの形

```
MSG_READER_RUN = 8
  -> [str peerPath]              origin。ファイルまたはフォルダ (§4.1 どおり)
     [str func]                  関数名 ("load")
     [u32 nFiles = 3]            per file: [str name][str body]
                                 name は "reader.py" / "viewer_import.py" /
                                 "run_adapter.py" の 3 語のみ (それ以外は拒否 —
                                 peer のディスク上の置き場所を client が指定する
                                 経路を最初から作らない)
  <- MSG_OK:
     [u32 outcome]               0 ok / 1 gate closed / 2 no python /
                                 3 could not start / 4 timed out /
                                 5 reader exited non-zero / 6 output unreadable
     [str err]                   outcome != 0 のときの一文 (§6 の文面)
     [str stderrText]            harness の stderr 丸ごと (summary・traceback・
                                 reader 自身の print)。成功時も运ぶ (§4.13.0 の
                                 パネルが出すものはこれ)
     [str pyProvenance]          "Python 3.11.4 (/usr/bin/python3), numpy 1.26.4"
     [str key]                   peer が発行した不透明なキャッシュ鍵 (16 進)。
                                 outcome == 0 のときだけ
     [str headerText]            .vstream の先頭〜"end" 行まで**逐語** (§5.2)

MSG_META / MSG_TILE               既存の全フィールドの後ろに追記 (append 規律):
  ... [str key][u32 node]         key 空 = reader 無し (今日の全 client が
                                  実質書いている状態)。node は木の pixel ノード番号

MSG_MEASURE                       MeasureReqHead.flags に MRF_READER = 2。
  ... rois の後ろ (MRF_RAW_RECIPE :2482 と同じ位置規律) に [str key][u32 node]
```

鍵を **peer が発行し client は引用するだけ**にする理由: 鍵の再計算実装が 2 つ
あればずれる (#71)。また鍵は peer のキャッシュディレクトリ内のみを指す 16 進
トークンで、パスを運ばない — client が peer の任意パスを開かせる第 2 の経路を
作らない。stat が動いた origin に対しては、**開いている doc は自分が束縛した鍵の
materialisation を読み続ける** (ローカルの「identity BEFORE the bytes」と同じ
意味論。再実行は明示の reload / 再 RUN だけ)。

### 4.3 MSG_TILE の原則は reader 産画素にも成り立つ

MSG_TILE は元から「復号済み画素を source dtype で返す。ファイルバイトは返さない」
(remote_proto.h :10-12, :118-123)。reader 経路での「source」は **reader が宣言した
配列の dtype** である — harness が 9 種 (`u1 i1 b1 u2 i2 u4 i4 f4 f8`) に正規化
済みで、wire の DType に全部載る (`b1 → DT_U8` は openNpy の既存表 serve.cpp :267
の写しであり、新しい対応を発明しない。u4/i4/f8 の f32 損失は既存の census が
そのまま数える)。§4.13.1 の 2 行目「**変換結果は回線を渡らない**」はこう実装
される: materialisation (.vstream) は peer に置かれたまま、渡るのはヘッダの
テキストと、画面が要る間引きタイルと、MEASURE の結果だけ。

---

## 5. peer 側の実行 — #44 の枠付きストリームがそのまま配布面になる

### 5.1 実行

RUN handler は `core/adapter.cpp` をそのまま使う (このために viewer 状態フリーに
作られている — adapter.h 冒頭)。期限は **300 s、ローカルと同じ定数** (session.inc
:1617/:1622)。期限も政策も wire に載せない — 政策はコードの居る側に置く。
interpreter は `adapter::findPython` (numpy import が probe)。peer には設定 UI が
無いので上書きは環境変数 `VIEWER_SERVE_PYTHON` (VIEWER_SERVE_PLUGINS と同じ
「peer 側の設定は env」の前例)。

client 側は **reader ジョブのスレッドが自前の session を開いて** RUN を投げる
(App::ReaderJob の既存の非同期形 :1606-1620 のまま。UI session に載せない理由は
:1877 の注釈そのもの — 300 s 級の往復で UI の枠を止めない・1 本の ssh stdin を
2 スレッドで枠詰めしない)。その session の idleTimeout は 300 s + 余裕に上げる。

### 5.2 配布 — ヘッダは渡り、blob は残る

harness の出力は凍結済みの枠付きストリーム v1 (`VIEWERSTREAM 1`、テキストヘッダ +
宣言順の生 blob、LE 固定・`_esc` 済み — adapter-transport-review A2/A4)。peer は
これをキャッシュファイルとして受け、

1. **木の検査**: A3 の `vnzCheckTree` 相当を**共有 TU に括って両バイナリから呼ぶ**
   (`parent` 上限・layer 語彙・conditions 長)。断り文がローカルの門と一字一句
   同じであること — #71 の規律。`VIEWERSTREAM 2` (set 入り) は v1 では名指しで
   断る (§7)。
2. **ヘッダを RUN 応答で逐語返す**。client は**既にある 1 つの木ビルダ**
   (`vnzBuild` + `VnzFetch` 継ぎ目、loader_npz.inc :1212) でそれを解釈する。
   層・軸・単位・note・cfa・range はローカルの reader オープンと同じコードを
   通るので、**同じ reader が local と peer で違う doc を作る余地が構造的に無い**。
   fetch だけが差し替わる: ローカル = キャッシュファイルの blob 読み、リモート =
   下の TILE。
3. **TILE は openRaw の形で読む**: `.vstream` の blob はノードごとに C-order 連続
   なので、`ServedFile` に dataOffset (ヘッダ長 + 先行 blob の和) と stride を
   立てれば `readNpyRegion` :301 が**無改造で** step 間引き・部分矩形を返す。
   headerless RAW (protocol 11) が敷いた道と同じで、reader 専用の画素ループは
   1 本も書かない。
4. **client の doc はリモート doc として着地する**: 最初の 1 枚は間引きタイル
   (openRemote :1902 の step 計算のまま)、zoom/pan で精細化、stack は n / N。
   フォルダ由来の巨大 stack を全解像度で引き寄せる経路は最初から無い。

### 5.3 キャッシュ — §4.12 の鍵の remote 版

```
key = H( peerPath,
         peer の stat (mtime, fsize),          ← stat は peer が持つ。client の
                                                  stat では NAS の二重マウントで
                                                  同一性が嘘になる
         sha256(reader.py), sha256(viewer_import.py), sha256(run_adapter.py),
         func, module VERSION (adapter::moduleVersion を運ばれた本文に適用) )
```

- 置き場: `~/.viewer-serve/reader-cache/` (env `VIEWER_SERVE_CACHE` で上書き)。
- **2 度目の RUN は Python を起動しない** — stat と鍵計算だけでヒットし、応答に
  「read from cache - the reader was not re-run」を返す (ローカル :1567 と同文)。
- 並行書き: 同一鍵は同一バイトを書く (鍵に stat と全ハッシュが入るため —
  adapter-transport-review A 系末尾の議論がそのまま成立)。壊れた・途中で切れた
  キャッシュは検査 (上の 1) で落として作り直す (session.inc :1571 と同じ自己回復)。
- 掃除: v1 は作らない (ローカルの adapter-cache と同じ)。**再訪条件**: 実地で
  キャッシュ肥大が報告されたら、両側同時に同じ方針で入れる。
- 一時ファイル (運ばれた .py) は run の成否に関わらず削除。キャッシュに残るのは
  `.vstream` だけ — peer のディスクに**コードを常設しない**ことが §3 の形を守る。

### 5.4 常駐ワーカ・共有メモリはここに入れない

#44 の決着どおり運び屋は**ファイル渡しで確定** (0.268 s / 151 MB 実測、パイプ段は
削除済み)。peer 側でも同じ理由がそのまま立つ (キャッシュがファイルを要求する、が
特に)。常駐ワーカ (起動 0.12-0.19 s / torch import 1.2-1.4 s の節約) は #45 の
W1/W2 が main に入った時点の再訪条件 (`adapter-transport-review.md` §3.1) に
peer 側も**同乗する** — この設計はそれを妨げる配線を作らない (RUN handler と
実行器の間は `adapter::run` 1 呼び出しで、差し替え点が既に 1 箇所)。

---

## 6. 失敗のワイヤ越し報告 — 3 種が client の画面でどう読めるか

RUN 応答の `outcome`/`err`/`stderrText` を `readerFinish` の文面**そのもの**に
写像する。文はローカルの関数から括り出して共有し (同じ失敗に 2 つの文を作らない)、
**peer で起きた失敗は機械を名指しする**。表示面はローカルと同じ 2 つ: toast に
一文、Reader パネル (`readerLastOut`) に全文。

| 失敗 | peer が返すもの | client の画面 |
|---|---|---|
| **Python が無い** | outcome 2 + findPython の `why` | `no Python to run the reader with on <host>: <why> (install numpy for it, or set VIEWER_SERVE_PYTHON where viewer-serve runs)` — ローカルの文 (:1548) + 機械名 + peer 側の直し方。**client の Settings を触らせない**: 直る場所は向こうである |
| **adapter が例外** | outcome 5 + stderr = traceback 丸ごと | パネルに**逐語** (§4.9「メッセージがそのままユーザーに出る」・§4.13.0「traceback を丸ごと出す」)。toast は最終行。行番号は運ばれた reader の本文と一致する — 運搬型 (§3) だから、client の手元のファイルのその行がまさに失敗した行である |
| **形が宣言と合わない** | outcome 6 + 共有 `vnzCheckTree` の文 | `the reader ran on <host> and returned, but the viewer could not read what it produced:` + ローカル門と同一の検査文 (:1674 の形)。両門同文は A3 の検査を共有 TU にすることで構造保証 |

付随 (同じ表の続きとして実装する): outcome 3 = `could not start the reader on
<host>: <fail>` + 実コマンド、outcome 4 = `the reader did not finish on <host>
(stopped after 5 minutes)`。**実行前の実コマンド表示** (§4.13 の約束) も remote で
生きる: client は送信前に `logMsg("reader (on <host>): python -u run_adapter.py
<spec> <path> --stream")` を残す — 実際に peer で組まれる argv と同形であること
自体を selftest が検査する。

**v1 で出来ないこと (名指し)**: 実行**中**の stderr の tail (ローカルはファイルを
tail する :1603)。RUN は一往復なので、進捗はパネルに「running on <host> …」、
全文は完了時にまとめて出る。**再訪条件**: #45 の制御路 (行ベース小メッセージ) が
入ったら進捗の中継をそこに載せる。

---

## 7. v1 の縁 — 断ると決めて記録するもの

| 断るもの | 文と理由 | 再訪条件 |
|---|---|---|
| **set 入りの返り値** (`VIEWERSTREAM 2`) | RUN 完了時に名指しで拒否: set の Ref は peer のパスを指し、役割の解決と着地は client の登記と絡む。「新しい版は読まずに断る」の既存規則がそのまま器になる | series/batch が実運用で通った後、`.npz` over link の「zip の中身一覧を返す動詞」(docs/npz-design.md) と**同じ回**で設計する — 同じ「ファイルの中の部分を指す」問いだから |
| **ssh:// での「手元で走らせる」逃げ道** | §4.13.1 は「手元で走らせる (ファイルが流れます) を明示的に選ばせる」と書いたが、**送・受どちら向きにも file 転送 op は存在せず** (remote_proto.h :129「there has never been a send-a-file op」)、作ることは「見えているものと数値だけが流れる」への原則級の変更。v1 の逃げ道は文で言う: `copy the file here (scp) and use the reader locally` | Python の無い peer に実地で繰り返し当たったら、そのとき初めて fetch op を独立の裁定に掛ける。**local:// では逃げ道は既に在る** (openRemote :1848 が openPath へ回すので、ローカル読みも Reader パネルもそのまま効く) — 失われているのは ssh だけ、と記録する |
| **remote reader doc の自動 reload / watch** | 既存裁定の合流: 自動 reload は peer stack に対して走らない (`watchAutoRefusal`)。手動 Reload は stat 変化 → 鍵不一致 → 明示の再 RUN を促す文 | rwatch の器 (remote finding) に reader 鍵の失効を載せる別件 |
| **peer 上の reader 常設・名前解決** | §3.2 の 4 理由。淡色行の hover/toast はその旨を言わない (存在しない機構を仄めかさない) | ssh でない転送の設計時 (§2 の再訪) に、allowlist 型と一緒に再考 |

---

## 8. 段階 — 各段が単独で main に入り、単独で意味を持つ

検証はすべて **local:// peer + `--remote-exe` の standalone viewer-serve** で行う
(rwatch/rtemporal の型)。この feature では特に必須である: 「reader が **peer の
プロセスの python で**走った」ことは、client が自分自身に喋る構成では証明でき
ない (rwatch.inc :5-8 と同じ理由)。

### stage 0 — 拒否文が事実を言う (**設計から独立。即日の Opus 仕事**)

- `imagefile::peerRefusal` の fall-through (:485-487) に一文を足す:
  `"a reader could read it, but readers do not yet run on the peer - the
  decision (docs/input-adapters.md §4.13.1) is that they run where the file
  lives, and that door is not built yet\n  copy the file here to use a reader
  today"`。G1 が headerless で踏んだ前例 (:454-456「The wording changes the day
  the wire can carry a recipe」) をそのまま踏む — **door が開く段 (stage 2) で
  この文は `choose a reader` に置き換わる**、と注釈に書き添える。
- **赤→緑**: `fmtgate` の peerRefusalFor 検査 (fmtgate.inc :431-442) に
  「未対応拡張子の拒否文が §4.13.1 と『not built yet』を言う」assert を先に足す
  (現行文には無いので赤) → 文を足して緑。
- verify-matrix G11 の行を「stage 0 済 (拒否が判断を言う)、実装は本設計」に更新。

### stage 1 — protocol 12: RUN が peer で走る (doc はまだ開かない)

- VERSION 12・`MSG_READER_RUN`・`--serve-readers`・`readerTooOldText`・
  `VIEWER_SERVE_PYTHON`・キャッシュ・§6 の outcome 全種。client 側は remote::Session
  に `readerRun()` を足すところまで (UI 配線なし)。
- **赤→緑** (`--rreader-selftest` 新設、先に書いて赤):
  R1 fixture の 5 次元 `.npy` + permute reader を RUN → outcome 0、stderr に
  `read 1 stack`、peer 側キャッシュファイル実在。
  R2 同じ RUN をもう一度 → 応答が cache 文、**キャッシュファイルの mtime が
  動かない** (= Python 不起動の観測)。
  R3 `VIEWER_SERVE_PROTOCOL=11` → client が**送信前に** readerTooOldText。
  R4 旗なし起動 → gate の一文 (R3 と別文であること)。
  R5 `VIEWER_SERVE_PYTHON=/nonexistent` → §6 の一文が host 名を含む。
  R6 raise する reader → traceback が応答に丸ごと居る。
  R7 (S,H,W) を Stack と名乗る reader → 両門同文 (ローカル `loadViewerStream` の
  文と strcmp 一致)。

### stage 2 — 開く: 単根の frame / stack が link 越しに doc になる

- META/TILE トレーラ・`openReaderCache` (openRaw 形)・RUN 応答ヘッダ →
  `vnzBuild`(VnzFetch = remote tile)・間引き着地・Inspector の
  `reader <spec> (ran on <host>)` 行・`rememberReader(url, spec)` (memo の鍵は
  **url 全体**。`readerFor(d->src->path)` :172 は remote doc の path = url なので
  既存コードがそのまま正しい)・stage 0 の文を `choose a reader` に差し替え、
  Browse の淡色行 → Reader パネルの導線を開く。
- **赤→緑**: R8 同じファイル・同じ reader を local と local:// peer で開き
  **画素バイト一致** (fmtgate の bit-identical 規律。同一機・同一 python なので
  張れる assert であり、異機の ULP 一致は約束しない —
  adapter-transport-review §5.1 の線のまま)。R9 dtype 表示一致 (`u2` は両側 `u2`)。
  R10 1600px 超の materialisation が step > 1 で着地し n / N が出る。
  R11 set 入り reader → §7 の一文。

### stage 3 — 木: series / batch が通る

- `[u32 node]` の実配線 (stage 2 では常に単根の pixel ノード)。series の
  conditions・単位・note がヘッダから両側同値で立つこと。
- **赤→緑**: R12 露光 8 × 16 枚の Series reader を peer 越しに開き、ローカルと
  doc 数・conditions 値・単位が一致。R13 ノード番号の範囲外 → 名指し拒否。

### stage 4 — memo / session: V25p の remote 版

- 復元: memo (url 鍵) が引ければ RUN → 開き直し (ローカル復元 :2691-2701 と同じ
  「memo が走らせる」)。引けなければ readerhint を**名指しだけ**して失敗報告
  (§4.12.1 裁定 C — hint は remote でも走らない)。
- **赤→緑**: V25p-r1 memo ありで復元 → doc が戻る・**adapter 実行回数が RUN
  キャッシュにより増えない**。V25p-r2 memo を消して復元 → hint 名入りの失敗行、
  実行回数 0。

### stage 5 — MEASURE: reader 産 stack を peer で測る

- `MRF_READER` + トレーラ。`FrameSource::init` (serve.cpp :1505) が
  openReaderCache を通るだけで、MOP_TEMPORAL_STATS / MOP_FRAME_ROI_STATS /
  plugin 系は無改造。
- **赤→緑**: rtemporal の型 — reader 産 stack の σ_t が独立 f64 参照と一致し、
  同 stack をローカル reader 経由で開いて測った値と**ビット一致**。

---

## 9. 決定の台帳と再訪条件 (集約)

| 決定 | 再訪条件 |
|---|---|
| 門は `--serve-readers` (起動者の同意、既定 閉) | shell 同等でない転送の実装時 |
| reader は client から運ぶ (3 テキスト、peer に常設しない) | 複数ファイル reader の実需要 → manifest 型 |
| VERSION 12 は FRAMING、client は送る前に数字で断る | — (この header の恒常規律) |
| 運び屋はファイル渡しの枠付き v1、peer でも同じ | #45 W1/W2 着地で常駐ワーカ再測 (§5.4) |
| キャッシュ鍵の stat は peer、鍵は peer 発行の不透明トークン | キャッシュ肥大の実地報告 → 両側同方針の掃除 |
| set は断る、ssh の「手元で走らす」は文で逃がす、進捗 tail は無し | それぞれ §7 / §6 に個別に記載 |
| stage 0 は独立に先行 (拒否文の 1 文 + fmtgate assert) | stage 2 で文を差し替え |
