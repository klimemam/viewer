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
| 3 | プロトコル | **FRAMING の版上げ** (7/8/11 と同種)。新 op `MSG_READER_RUN = 8`。META/TILE/MEASURE は**末尾追記のトレーラ** (`[str key][u32 node]`)。client は **送る前に** HELLO の数で断る (`readerTooOldText` / `measureKeyedTooOldText`) — 古い peer はトレーラを黙って読まないので、#124 型の「正しいラベルの下の違う画素」が起きうるため (§4.2)。**出荷は 1 版ではなく 3 版**: 12 = RUN と META/TILE のトレーラ (PR #218)、13 = `MSG_NPZ_SCAN` (PR #221)、**14 = MEASURE の鍵トレーラ**。MEASURE のビットの綴りは **`MRF_KEYED`** (値 2) —— 鍵は materialisation を指し、reader はその出自の 1 つに過ぎない。以下 §4 が現在形で書く「VERSION 12 / `MRF_READER`」は**当初案の綴り**で、確定は §8 stage 3 注 ② と stage 5 注 ①、台帳は §9 |
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

## 4. プロトコル — VERSION 12 (当初案の版番号。出荷は 12/13/14 — §0 の 3)

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
                                  (出荷時の綴りは MRF_KEYED、値 2 のまま。版は
                                   14 —— stage 5 注 ①。node は「peer が発行した
                                   その配列の番号」であって木の index ではない:
                                   reader では両者が一致し、container では
                                   SCAN が出した ZIP entry である — §10.5)
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

### stage 1 — protocol 12: RUN が peer で走る (doc はまだ開かない) — **済**

> 実装 2026-08-17 (issue #180 stage 1-2)。試験は `--rreader-selftest`
> (`core/selftest/rreader.inc`)。設計との差分 3 点:
> ① 鍵の中身はハッシュ関数が sha256 でなく `adapterHash` と同じ 64-bit FNV
>    (ローカルのキャッシュ鍵と 1 つの関数に揃えた)。入力は設計より強く、
>    reader の**本文そのもの**を混ぜているので `moduleVersion` は不要。
> ② `VIEWER_SERVE_PYTHON` は peer では**厳格**(指定が失敗したら PATH に
>    落ちない)。peer 側には設定窓が無く、黙って別の python で走った結果は
>    再現できないため。
> ③ 木の検査を共有 TU に括る件は `core/vstream.h` の `vns::checkTree` +
>    `vns::scanHeader`。ヘッダの**残り**(note/cfa/range/…) は client だけが
>    使うので共有していない —— 代わりに client の 2 経路 (キャッシュファイル /
>    peer が返したヘッダ本文) が `vnsParseHeader` 1 本を通る。



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

### stage 2 — 開く: 単根の frame / stack が link 越しに doc になる — **済**

> 実装 2026-08-17 (同 PR)。META/TILE の末尾は設計の `[str key][u32 node]` に
> **フラグ語 1 つを前置**した (`rp::ReqTrailer`): v11 の「残りバイトがあれば
> recipe」規則は任意ブロックが 2 つになると成立せず、鍵が geometry として
> 読まれる事故が起きうる。v12↔v12 のときだけ書き、両端とも
> `servedVersion()` で門を張るので `VIEWER_SERVE_PROTOCOL` の継ぎ目は生きている。
>
> 設計との差分 2 点:
> ① doc は `vnzBuild` が作る**ローカル doc** (間引き済み画素を保持) であり、
>    `openRemote` が作る remote doc ではない。よって zoom での精細化
>    (`pumpRemoteFetch`) は**繋いでいない** —— 着地は §5.2.4 どおり間引き
>    (`step = ceil(max(w,h)/1600)`) で、そこから先の要求は出ない。
>    木ビルダを 1 本に保つ (§5.2.2) 方を採った結果で、精細化を足すなら
>    stage 3 の木配線と同じ回に `S.remoteUrl` を立てるのが素直。
> ② R10 (1600px 超) は書いていない。間引きの経路は R8 と同じ 1 本で、
>    step の計算だけが違う。
>
> 併せて stage 0 の拒否文を `choose a reader` に差し替え (`core/imagefile.cpp`)、
> fmtgate F4d2 をその裏返しに更新、Browse の淡色行 →
> `openReaderPicker(url, …)` → Reader パネルの Load → **自前 session を持つ
> スレッドで RUN** (§5 の「UI の枠を止めない」) を繋いだ。


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

### stage 3 — 木: series / batch が通る — **済**

> 実装 2026-08-17。**protocol 13** (§10.7 の規律どおり: 12 は #218 で出荷済みなので
> npz の動詞は 13 に乗った)。木そのものは stage 2 の継ぎ目がそのまま通した ——
> `[u32 node]` は最初から木のノード番号で、peer 側 `openReaderCache` は blob を
> node で引いている。stage 3 で足したのは **試験と、範囲外の拒否が名指しである
> ことの確認** (R12/R13) と、下の精細化配線である。
>
> 設計との差分:
> ① stage 2 の注記①(精細化未配線)を**この回で解消した**。`FrameSource` に
>    `remoteKey` / `remoteNode` / `remoteKeyKind` を足し、`RFetchJob` /
>    `RFetchDone` にも同じ 3 つを載せた上で、木が着地したあと
>    `remoteTreeRefine` が node ごとに `S.remoteUrl` / `remoteFrame` /
>    `remoteStep` を立てて `requestFullRemote` を呼ぶ。精細化の TILE は
>    step 1 の同じ要求なので新しい経路は 1 本も無い。
>    併せて `vnzBuild` に `peerPixels` を足した —— リンク越しに来た画素は
>    **間引き済み**で、identity tuple に step の欄が無い以上ソース登記に
>    入れてはならない (reader 出力を登記しない §6.2 の理由の一段下)。
> ② `MRF_KEYED` への改名は**しなかった** —— `MRF_READER` は 1 度も書かれて
>    いない (MEASURE のトレーラは stage 5)。代わりに **既に出荷済みの綴り**
>    `rp::RQ_READER` → `rp::RQ_KEYED` と `remote::ReaderRef` → `KeyedRef` を
>    直した。値 2 は不変なのでワイヤは 1 バイトも動かない。stage 5 が
>    MEASURE のビットを足すときは最初から `MRF_KEYED` と綴る。

- `[u32 node]` の実配線 (stage 2 では常に単根の pixel ノード)。series の
  conditions・単位・note がヘッダから両側同値で立つこと。
- **赤→緑**: R12 露光 8 × 16 枚の Series reader を peer 越しに開き、ローカルと
  doc 数・conditions 値・単位が一致。R13 ノード番号の範囲外 → 名指し拒否。

### stage 4 — memo / session: V25p の remote 版 — **済**

> 実装 2026-08-17 (issue #180 stage 4)。試験は `--rreader-selftest`
> (`core/selftest/rreader.inc` の V25p-r0〜r2)。**ワイヤは 1 バイトも動かして
> いない** —— 復元は stage 1〜2 の RUN 経路をそのまま呼ぶだけで、変更は
> `core/app/session.inc` の復元分岐 1 箇所に閉じた。
>
> 設計との差分 3 点:
> ① **保存側は無改造で既に正しかった**: memo の鍵が url 全体 (stage 2) なので
>    `readerhint` の既存条件 (`member` が `__pixels_` で始まる → `readerFor(path)`)
>    が remote doc にもそのまま当たる。V25p-r1a/r1b はその assert。
> ② memo 枝は memo **だけ**でなく行の `member` (`__pixels_`) も見る。ローカル
>    の門は memo 単独で判断するが、link の向こうでは同じ url を native で開く方
>    (Browse) が常態で、「その doc が reader 産か」を知っているのは行の方だから。
> ③ V25p-r1 の「実行回数が増えない」は client 側 `g_adapterRuns` では自明に 0
>    (client は Python を起動しない)。実質の観測は **peer の materialisation の
>    mtime が動かない** (stage 1 R2 の観測の一段上) で、両方を張っている。
>    V25p-r2 の「hint は依頼もしない」は `bytesReceived()` が 1 バイトも動かない
>    こと + peer のキャッシュ不変 —— 拒否は `ensureUiSession` より手前なので、
>    接続すら起きない。

- 復元: memo (url 鍵) が引ければ RUN → 開き直し (ローカル復元 :2691-2701 と同じ
  「memo が走らせる」)。引けなければ readerhint を**名指しだけ**して失敗報告
  (§4.12.1 裁定 C — hint は remote でも走らない)。
- **赤→緑**: V25p-r1 memo ありで復元 → doc が戻る・**adapter 実行回数が RUN
  キャッシュにより増えない**。V25p-r2 memo を消して復元 → hint 名入りの失敗行、
  実行回数 0。

### stage 5 — MEASURE: reader 産 stack を peer で測る — **済**

> 実装 2026-08-17 (issue #180 stage 5)。試験は `--rmeasure-selftest`
> (`core/selftest/rmeasure.inc`)。設計との差分 4 点:
> ① **綴りは `MRF_KEYED`、値は 2**(§10.5 の指示どおり)。ただし**版は 14**であって
>    12 ではない。§10.7 が固定した規則がそのまま決める —— 12 も 13 も**出荷済み**
>    (PR #218 / #221) で、13 を名乗る peer は MEASURE に鍵を知らない peer である。
>    番号を共有すると「送る前に数字で断る」が書けなくなる。定型は 6 本目の
>    `rp::measureKeyedTooOldText`。
> ② **鍵付き要求はパスを 1 本も送らない**(`nPaths = 0`)。§4.2 が META で決めた
>    「path は送らない」をそのまま MEASURE に適用した結果で、これが版番号の
>    実質でもある: パスを添えていたら v13 peer は**コンテナ丸ごとの σ_t** を
>    正しい見出しの下に返していた (#124)。添えないので v13 は
>    "bad MEASURE header" と答え、それは読めない文なので client が先に断る。
> ③ **plugin 系は無改造ではなかった。** `runStackAnalyzerOn` / `runFrameAnalyzerOn`
>    は `paths[0]` を**人間が読む名前**として使う (「… "cube.npy" has 2 of 6
>    frames」)。鍵にはその名前が無く、16 進トークンを refusal に出すのは名前では
>    ない。よって v1 で鍵を運ぶのは **MOP_TEMPORAL_STATS / MOP_FRAME_ROI_STATS の
>    2 つ**で、MOP_ANALYZER / MOP_PLUGIN_ANALYZE / MOP_SET_FOLD は**名指しで断る**
>    (hasRecipe が既に持っている拒否と同じ形・同じ位置)。**再訪条件**: peer が鍵に
>    人間の読める名前を持てるようになった時 (reader 側の `.readersrc` 相当) に、
>    plugin 系へ広げる。
> ④ **client 側の配線が必要だった。** 設計は peer 側だけを書いていたが、reader 産
>    stack は `vnzBuild` が作るローカル doc の集まりなので `SeqInfo::remoteUrl` が
>    空で、`serverComputes` が false を返して**そもそも測定要求が出なかった**。
>    subject は doc 側 (`FrameSource::remoteKey/remoteNode/remoteKeyKind`) にあり、
>    `stackKeyedSubject` がそれを引く。`SeqInfo` に鍵を写さない理由は Watch:
>    `si.remoteUrl` を立てると reader 産 stack が peer stack として watch 対象に
>    入ってしまい、それは §7 が別件と記録した話だから。
>    ついでに直った既存の欠陥: **.npz member の stack は今日も server temporal を
>    投げていて、url = コンテナなので "not a .npy file" が返っていた**(赤の観測)。

- `MRF_KEYED` + トレーラ。`FrameSource::init` (serve.cpp) が `openKeyed`
  (= META/TILE と同じ 1 本) を通るだけで、MOP_TEMPORAL_STATS /
  MOP_FRAME_ROI_STATS の算術は無改造。
- **赤→緑**: rtemporal の型 — reader 産 stack の σ_t が独立 f64 参照と一致し、
  同 stack をローカル reader 経由で開いて測った値と**ビット一致** (M1)。
  npz member でも同じで、しかも**両者が互いにビット一致**する (M2 — 同じ画素を
  2 つの鍵発行者から測っているので、ワイヤがどちらが発行したかを知らないことの
  証明になる)。旗を閉じた peer では reader 産 doc が存在しないので測るものが無く、
  member の測定だけが通る (M3 — 旗はコードを守る、データではない)。v13 peer は
  送信前に断られる (M4)。文法の縁 — 第 2 の集計 op・set fold の拒否・recipe との
  同居拒否・未発行の鍵 (M5)。

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
| MEASURE の鍵は **版 14**、鍵付き要求はパスを送らない (`nPaths = 0`) | — (§10.7 の規則の帰結。この header の恒常規律) |
| 鍵を運ぶ MEASURE op は集計 2 つだけ、plugin/set は名指しで断る | peer が鍵に人間可読な名前を持てるようになったら plugin 系へ (stage 5 注 ③) |

---

## 10. 追記 (2026-08-17, Fable) — remote .npz は stage 3 に乗る (issue #217)

**乗る、と判断する。** ただし #217 の言い方に一点訂正がある (§10.1): ordinary npz
に「木」は無い。乗るのは木のコードではなく、**ワイヤの継ぎ目**である。

状況: ユーザー報告「.npz がいまだに remote だと gray out」(#217)。拒否は判断済み
(`core/imagefile.cpp:438`「the peer serves one array per file, not a container」)
で、**reader とは無関係** — .npz は native 形式であり、欠けているのは「コンテナ
(複数配列) がリンクを渡る」機構だけ。`docs/npz-design.md` §2.4 (:91) は既にその
動詞を名指ししている:「リモートでは peer に zip の中身一覧を返す動詞が要る —
プロトコル追加。ローカル先行、リモートは後続で良い」。後続の番が来た、というのが
本追記である。

### 10.0 決定 5 件を先に

| # | 問い | 決定 |
|---|---|---|
| a | peer 側で npz の木を作るのは誰か | **誰も作らない。** peer は列挙と materialise だけ。木・分類・picker は client の既存 1 経路 (§10.2) |
| b | member 選択 UI | **同じ picker、同じ入口** — drawNpzPickModal と loadNpz の規則すべてを共有 (§10.3) |
| c | :438 の拒否文 | .npz は **peerServesDeclared に入り、peerServes には入らない** (protocol 11 が headerless で割った線)。:438 の文は「member 一覧から開く」事実に書き換わり、旧 peer には presend の `npzTooOldText` (定型 5 本目) が断る (§10.4) |
| d | MRF_READER トレーラとの区別 | **しない。** [str key][u32 node] の意味を「peer 上の materialisation の中の 1 配列」に固定する。reader は key の出自の一つに過ぎない (§10.5) |
| e | キャッシュ鍵 | `H("npz", peerPath, peer の stat)` — **SCAN 時に peer が発行**、member は node で選ぶ。reader 鍵とは kind 語で族が分かれる (§10.6) |

`--serve-readers` は **npz に関与しない**: §2 の門が守るのは「他人のコードがこの
機械で走る」ことで、npz で peer に走るのは viewer 自身のコードだけである。旗の
閉じた v12 peer でも SCAN は答える。

### 10.1 #217 の言い方への訂正 — 「木」は半分だけ正しい

#217 は「remote .npz は peer が native に開いた npz の木」と言うが、ローカルの
ordinary npz に木は無い: `npzScan` (loader_npz.inc :617) が member を**フラットに**
分類し、picker (:2213-2224) が選ばせ、`npzOpenMember` (:758) が 1 member ずつ
開く。`vnzBuild` を通るのは `__viewer` 判別子を持つ viewer container だけ
(:2136-2150)。したがって「stage 3 に乗る」の正確な中身は:

- **両方の顔が共有する**のはワイヤの継ぎ目 — SCAN 動詞 + [key][node] トレーラ +
  peer 側 materialisation キャッシュ + openRaw 形の TILE (§5.2 の 3 と同じ道)。
- **vnzBuild に触るのは container の顔だけ**で、その形は stage 3 の reader 木と
  同一 (「宣言は逐語で渡り、木のビルダは client の 1 つ」§5.2 の 2)。
- ordinary npz の顔が共有するのは npzScan → picker → 1 member 開き、という
  **client の既存 1 経路**。これも「コードは 1 つ」だが、木のコードではなく
  分類と picker のコードである。

### 10.2 (a) peer は木を作らない — 列挙と materialise だけ

**コンパイル単位の現実** (確かめた): viewer-serve は serve_main.cpp + serve.cpp +
imagefile.cpp + 各画像 reader + miniz (CMakeLists.txt :613-618)。loader_npz.inc は
「core/main.cpp から #include される断片」(loader_npz.inc :1) で、**peer には
居ない**。そして vnzBuild は app.images と seq/series の登記を直接触る — App の
無い peer が呼べる関数ではない。「peer が loader_npz の vnzBuild を呼ぶ」形は
最初から成立しない。

peer に足すのは **`MSG_NPZ_SCAN = 9`** (版の置き場は §10.7):

```
MSG_NPZ_SCAN
  -> [str peerPath]
  <- MSG_OK:
     [str key]        materialisation 鍵 (§10.6)。peer 発行・不透明
     [u32 kind]       0 ordinary / 1 viewer container (+ __viewer の版)
     [u32 nMembers]   per member: [str name][u32 ndim][i64 dims...]
                      [str descr][u64 usize][str err]
                      [u32 nInline][bytes]     小さい値 (scalar / 文字列 /
                                               短い 1-D) は npy バッファ逐語
     kind 1 のとき: 予約 member (__pixels_ 系以外) の npy バッファ逐語
```

- zip の central-directory 歩き (`npzList`) と npy ヘッダ覗き (`npyPeekHeader`)
  は**共有 TU に括って両バイナリで compile** — §5.2 の 1 が vnzCheckTree に既に
  決めた扱いそのもの。miniz は serve に**既にリンク済み** (serve.cpp :28、今日は
  タイル圧縮 :1346 に使っている) なので、新しい依存は増えない。
- **分類 (RImage/RAxis/RMeta/RBad の語彙) は client の npzScan 1 箇所のまま**:
  SCAN が返すのは事実 (name・shape・descr・usize・展開可否・小さい値) で、役割は
  その事実から client が判定する。picker の行の文言が local と peer で一字も
  違えない (#71/#148) ことの構造保証である。peer が担うのは bytes を持つ側にしか
  できないことだけ — 展開できるかの検査 (「truncated zip member」:425/:453 の文
  ごと共有 TU へ) と、値の同梱。
- 同梱する 1-D 値の上限は **frame 天井 1<<20** (serveLayout :204-208 の既存天井)。
  ローカル npzScan の cap は 1<<24 (:711) だが、2^20 を超える 1-D はどの stack の
  軸にも成れない (frames ≤ 2^20) ので、**共有化の際にローカル側も 1<<20 に締める**。
  観測可能な差は「軸候補と呼ばれるが決して昇格しない member」の分類文言だけ。
  矛盾の名指し: これは既存コード (:711) の cap 変更であり、本追記が決める。
- container の予約 member は「小さい」(word・数値・ベクトル — loader_npz.inc
  :887-888) ので逐語で乗る。条件 2^20 × f8 = 8 MB が最悪で 64 MB 枠 (serve.cpp
  :2623) に収まる。`__pixels_<i>` は乗らない — それが渡らないための本設計である。
  版の拒否 (`VIEWER_NPZ_VERSION` :851、3 は名指しで断る) は client の既存文のまま
  — 検査の門は 1 つ、が §5.2 の規律。

#### 10.2.1 一通の SCAN 応答の天井 — 出荷時の値 (#221 review、#180 codex review で訂正)

**per-member の規則は応答を縛れない。** `nz::INLINE_MAX_BYTES` は 1 member を
8 MiB まで通し、正当な container が member を何百個持つことはある — 2^20 要素の
f8 軸 68 本は 8 MiB × 68 = **544 MiB**、どこも壊れていないファイルの応答である。
client (core/remote.cpp :405) は 512 MiB を超える応答を `oversized reply from the
peer` で断る。**転送についての文が、ファイルについて、人に届く** — その人に打つ手は
無い。だから合計にも所有者が要る。持つのは**メッセージを詰める側**、すなわち peer。

**天井は 256 MiB** (`rp::NPZ_SCAN_INLINE_MAX`)。client の上限のちょうど半分で、
これに収まった応答が `oversized` になる経路は無い。`VIEWER_SERVE_NPZ_SCAN_MAX`
(bytes) が上書きする — 256 MiB を踏む fixture は 256 MiB であり、算術の比較を
証明するために 1/4 GB を書く selftest は誰も走らせない、という理由だけのために
在る。最初の scan で 1 度読み、peer は生涯 1 つの数を答える。

**天井が数えるのは応答の全体であって、値ではない (#180 codex review の訂正)。**
最初の実装は `f.bytes` だけを数えたので、**name が予算の外**にあった。zip の
member 名は 16-bit 長 — 60,000 文字 × 5,000 member は name だけで 300 MB になり、
値の合計 238.4 MiB は 256 MiB を通り、ワイヤには 524.5 MiB が出て、人には
`oversized reply from the peer` が返った。数え落としのある天井は天井ではない。

数えるのは、**inflate と `Buf` 構築より前に**、overflow-safe (減算のみ) で:

| 何 | bytes | 置き場 |
|---|---|---|
| `[str key][u32 kind][u32 version][u32 nMembers]` | `4 + 16 + 4 + 4 + 4` = **32** | `rp::NPZ_SCAN_REPLY_FIXED` |
| member ごとの固定 field (`nameLen`/`usizeLo`/`usizeHi`/`entry`/`errLen`/`whole`/`nBytes`) | **28** | `rp::NPZ_SCAN_FACT_FIXED` |
| member ごとの可変 — `name` + `err` + `bytes` | 実長 | `nz::readFacts` |

合計は `rp::Header` の 12 byte を含まない (client が縛るのは u32 の payload 長)。
`used` が `max` を超えないことは reserve が守る不変条件なので `max - used` は
wrap せず、`used + take` は形にすらならない。予算は **inflate の前に** 引かれる
— 断ってから解凍代を払うのは、天井が防ごうとしている障害そのものである。

**超えたら、ファイルは丸ごと断る** (`MSG_ERR`)。行を間引いて収める道は無い:
行の欠けた picker は嘘をつく listing である。文は member 名・そこまでの合計・
天井・ファイル名を名指しし、`key` は**発行されない** (断ったファイルについて
client が後から引ける材料化物を残さない)。応答を組み立てた後にも `Buf` の実寸と
予算を突き合わせ、食い違えば送らずに断る — 到達しない検査だが、外れたときの
代償が「断られたファイル」ではなく「終わった session」だからである。

赤→緑: rnpz **R20**(値の合計)・**R21**(name を含む正確なワイヤ総量、実寸一致まで)。

### 10.3 (b) member picker — 同じ画面、同じ入口

- **同じ modal**: `drawNpzPickModal` (sequence.inc :2851) は app.npzPick の行を
  描くだけで、行が zip バイトから来たか SCAN 応答から来たかを知らない。npzScan を
  「zip から事実を得る」層と「事実から行を作る」層に割り、後者を両経路が共有する。
  パス表示 (:2864 dispPath) は url をそのまま出す。
- **loadNpz の規則が全部ついて来る**: 画像 member 1 つなら dialog 無し (:2190)、
  dialog は 1 度に 1 枚で残りは queue (:2213)、軸候補は「長さ == frames」の同じ
  判定、軸の名前と単位はユーザー確定。remote 用の第 2 の規則集は作らない。
- **入口**: openRemote (open_dispatch.inc :1847) の門が .npz を SCAN の道へ回す —
  META より前 (コンテナに 1 つの geometry は無いので、META では答えられない)。
  local:// は今日と同じく :1848 が openPath へ回す (§7 の記録どおり、ローカルの
  picker がそのまま出る) — この道は ssh:// の道である。
- **開く**: 選ばれた member ごとに stage 2 の着地 (間引きの最初の 1 枚 :1902 の
  step 計算、stack は n / N、zoom/pan で精細化)。TILE は [key][member index] で引く。
- **復元**: src->path = url、src->member = 配列名 (npzOpenMember :769 と同じ器が
  remote doc にもある)。session 復元は onlyMember の規則 (:2115-2117) ごと共有 —
  SCAN して名指しの member だけを picker 無しで開く。memo / readerhint は無関係
  (native であり、走るものが無い)。

### 10.4 (c) 拒否文と灰色の解け方

- .npz は **peerServesDeclared (imagefile.cpp :414) に入る**: Browse の行
  (browse/panel.cpp :123) が点き、double-click が SCAN 経由で開く。**peerServes
  (:379) には入れない**: one-click preview は「1 つの geometry を持つファイル」の
  約束で、コンテナはそれを持たない。protocol 11 が headerless で割った線 (行は
  生きる / preview の門は広げない、fmtgate F4d が**対で** assert する形) をそのまま
  踏む。
- **imagefile.cpp :438 の文は stage 3 で書き換わる**。「the peer serves one array
  per file, not a container」は door が開いた日に**偽になる文**なので残せない —
  存在する door を否定する拒否は G11 の再演である。残る真実で言い換える:
  `".npz is a container and opens from its member list - this request addressed
  the whole file as one array\n  open it from Browse (the peer lists the
  members), or copy it here"` (最終文言は実装時、R18 が事実を assert する)。
  このブランチに残る客は、丸ごとの .npz を 1 枚として指した要求への peer 側の
  断り (serve.cpp :459 経由) だけになる。
- **旧 peer は client が送る前に数字で断る**: `formatServable` (remote.cpp
  :583-596) が pictureTooOldText でやっている形の 5 本目、`npzTooOldText`。
  remote_proto.h の定型 (:736/:750/:767 + §4.1 の readerTooOldText) に並ぶ。
- 名指しする隣人 (本追記の範囲外): serve.cpp :488 の .exr named-parts 拒否
  「the link addresses a file, not a part of one」— [key][node] はまさに
  『part of one』を言うワイヤの語彙なので、この拒否は stage 3 の日に再訪**可能**に
  なる。ここでは開けない (.exr の named part は SCAN の形も picker も別物)。
  再訪条件: remote .npz が通った後、.exr の layer を link 越しに開きたい実報告。

### 10.5 (d) reader トレーラとの区別 — しない

§4.2 のトレーラ [str key][u32 node] の意味を 1 つに固定する: **key は peer 上の
materialisation を、node はその中の 1 配列を指す**。reader RUN も npz SCAN も
key の**発行者**であり、ワイヤはどちらが発行したかを知らない。native npz が
reader 鍵を持たないことは問題にならない — 鍵は reader の鍵ではなく
materialisation の鍵だからである。reader である事実は provenance (Inspector の
reader 行、stage 2) に居り、番地付けには居ない。

矛盾の名指し 2 件 (どちらも未実装の本文なので、実装はこの追記の読みで行う):

- §4.2 の注「key 空 = reader 無し」は狭い。正しくは「**key 空 = path そのものが
  origin**」— reader 無しでも npz の TILE は key を持つ。
- **`MRF_READER` という綴りは stage 5 で嘘になる** (native npz の stack を測る
  MEASURE に reader はどこにも居ない)。値 2 は §4.2 のまま、綴りは実装時に
  **`MRF_KEYED`** とする。まだ 1 バイトも出荷されていないから、この改名は無料。

node の意味は**コンテナ自身の番地**: .vstream = 木の pixel ノード番号 (§4.2 の
まま)、npz = SCAN 応答の member index。member **名**は META/TILE に乗らない —
名→番地の束縛は SCAN の応答で 1 回だけ起き、client が peer の任意 path の任意
member を SCAN 無しで指す第 2 の経路を作らない (§4.2 の鍵の論法と同じ)。

### 10.6 (e) キャッシュ鍵と identity

```
key = H( "npz",                        ← kind 語。reader 鍵 (§5.3) と族が分かれ、
         peerPath,                        同じキャッシュ根に同居して衝突しない
         peer の stat (mtime, fsize) )  ← stat は peer が取る (§5.3 の NAS 論拠のまま)
```

- 発行は SCAN 時に 1 つ、member の選別は node。reader 鍵に居た reader ハッシュ・
  func・module VERSION は**入らない** — 走る Python が無いから。キャッシュ根は
  1 つ (§5.3 の置き場に同居する。根が 2 つ = 掃除方針が 2 つ)。
- **materialise は遅延で、member 単位**: (key, node) の最初の TILE がその member
  を 1 本だけ inflate してキャッシュに置く。40 member の npz で 1 member 開くため
  に 40 本 inflate しない。以後の TILE / MEASURE は openRaw (serve.cpp :568) と
  同じ「offset + stride の ServedFile」で readNpyRegion :301 が無改造で読む —
  §5.2 の 3 と同じ理由で、npz 専用の画素ループは 1 本も書かない。
- **identity BEFORE the bytes はリンク越しでこう守る**: inflate の直前に peer が
  再 stat し、SCAN 時の tuple と違えば**名指しで断る** (`"the file changed on the
  peer since its members were listed - reopen to rescan"`)。ローカルは zip 丸読み
  が束縛する (loadNpz :2121-2126) が、remote は読む瞬間が開いた瞬間より遅い —
  黙って新バイトへ再束縛するより、断って再 SCAN させる方が強い。
- **stored member を zip offset から直に serve する形は退けた**: 展開コストは
  ゼロだが、zip の in-place 上書きが「開いている doc の画素」を identity の下で
  差し替える — :2121 が名指しするまさにその窓である。stored も copy して鍵付き
  ファイルにする (1 回の read+write、ローカルの zip 丸読みと同じコスト級)。
  再訪条件: 実運用の multi-GB stored member で copy が痛んだ実報告 — その時は
  fd 保持か読み口検証の別設計を、identity を割らずに立てる。

### 10.7 版とワイヤの置き場 — §7 の「同じ回」束は解く

`MSG_NPZ_SCAN = 9` は **VERSION 12 の定義に同居**する — v12 は未出荷なので番号は
無料である。規律の名指し: stage 1 が npz より先に main へ入って**出荷された後**に
この動詞を足すなら、それは 13 である (「送る前に数字で断る」を成立させる数が
それだから)。

矛盾の名指し: §7 の表 1 行目は「.npz over link の zip の中身一覧を返す動詞」を
set (VIEWERSTREAM 2) の設計と**同じ回**に束ねた。その束は解く — 動詞の設計は
#217 (ユーザー報告 = 再訪条件の到来) で今ここに来た。束ねた根拠「同じ『ファイル
の中の部分を指す』問い」は [key][node] という**答えの共有**として残る。**set の
側は §7 のまま据え置き** — set の Ref が peer の path を指し役割の解決が client
の登記と絡む問題は、この追記の何にも答えられていない。

### 10.8 stage 3 の受け入れ条件と赤→緑 (追加分) — **済**

> 実装 2026-08-17 (stage 3 と同じ PR)。`MSG_NPZ_SCAN = 9` / **VERSION 13** /
> `npzTooOldText` / peer 側の遅延 materialise / `.npz` は
> `peerServesDeclared` へ / `imagefile.cpp` の拒否文を書き換え。
> 試験は `--rnpz-selftest` (`core/selftest/rnpz.inc`, R14〜R17) と
> `fmtgate` **F4g** (R18)。
>
> 設計との差分 4 点:
> ① **共有 TU は `core/npzfile.h`** (`nz::`)。zip 歩き・inflate・npy ヘッダ覗きに
>    加えて **「member の事実」(`nz::Fact`) を作る規則そのもの**を共有した ——
>    分類 (`npzClassify`) は client の 1 箇所のまま、という §10.2 の線は動かして
>    いないが、「どの member の値を運ぶか」(`nz::wantsValues`) は両側が同じ 1 本
>    でないと行の文言が割れるので、そこまでを共有側に置いた。
> ② **member の事実は npy ヘッダ**を**逐語**で運ぶ (フィールド分解ではない)。
>    §10.2 の素描 `[str descr][u32 ndim][i64 dims...]` には `fortran_order` が
>    無く、それが落ちると fortran 順の member の role が両側で割れる。client は
>    peer が覗いたのと同じバイトに `nz::peekHeader` を掛ける。
> ③ **鍵 → origin の対応は peer のキャッシュに小さな控え (`<key>.npzsrc`)** を
>    書く。プロセス内 map では足りない —— 精細化の TILE を投げるのは
>    `rfWorker` の**別プロセスの peer** であり、SCAN を見ていない。
> ④ **`local://` の .npz は今日どおりローカルの戸へ落ちる** (§10.3 の記録どおり)。
>    SCAN の道は ssh:// の道で、試験は `openRemoteNpz` を直接叩いて両側を
>    比較する (fmtgate bothWays と同じ型)。



stage 3 の完了条件に足す: **remote .npz が member 込みで開く — reader 不要の
native コンテナとして、SCAN + [key][node] で**。

| # | 赤→緑 |
|---|---|
| R14 | fmtgate の bothWays の型 (standalone peer への session 直叩き、fmtgate.inc :690〜) で multi-member .npz: SCAN の行がローカル npzScan と**同じ role・同じ文言**を出し、選んだ member の TILE 全画素がローカル open と**ビット一致** (同一機なので張れる assert — R8 と同じ線) |
| R15 | 画像 member 1 つの npz → dialog 無しで開く (:2190 の規則が remote でも同文で成立) |
| R16 | `__viewer` container を peer 越しに開き、ローカル open と doc 数・conditions 値・単位が一致 (R12 の npz 版 — R12 は reader 産、R16 は npz 産、**通る木のコードが同じ 1 つ**であることの証明) |
| R17 | `VIEWER_SERVE_PROTOCOL=11` → client が**送信前に** npzTooOldText / SCAN 後に peer 側で zip を上書き → 次の inflate が再 stat で名指し拒否 (§10.6 の文) |
| R18 | fmtgate: .npz の行が peer で生きる + preview の門は広がらない (F4d の対 assert の npz 版) + :438 の新文が「member 一覧」を言い、旧文「one array per file」を言わない |

### 10.9 台帳 (追加分)

| 決定 | 再訪条件 |
|---|---|
| npz は stage 3 の継ぎ目に乗る: peer は列挙と materialise、木・分類・picker は client の 1 経路 | — |
| トレーラは 1 本 (綴りは MRF_KEYED に直す)、key = materialisation、reader は出自の 1 つ | — |
| .npz は peerServesDeclared に入り peerServes に入らない (preview 無し) | 単一 image member npz の preview 実需要 |
| stored member も copy して materialise (zip offset 直 serve は identity で退けた) | multi-GB stored member で copy が痛んだ実報告 |
| §7 の「同じ回」束は解く: npz 動詞は今 (#217)、set は据え置き | set は §7 の再訪条件のまま |
| .exr named parts (serve.cpp :488) は同じ語彙で再訪可能になるが、ここでは開けない | remote .npz 通過後、.exr layer の実報告 |
