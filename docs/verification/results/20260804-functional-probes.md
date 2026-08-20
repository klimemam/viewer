# 2026-08-04 機能 probe（観測点）検証結果

- 対象コミット: `67e626cd073dfa66ece118690695a447c263afa2`
- ブランチ: `verify-probes`
- 実行環境: Windows / MinGW（実行機の識別情報は記録されていない）
- 種別: 実施記録。後日の実装状態に合わせて書き換えない。
- 再実行仕様: [../functional.md](../functional.md)

## 1. 追加した観測面

`srcMapDump()` と `--srcmap-selftest <dir>` を追加し、membership ごとに
`uid / seq / idx / src=#K / refs / rev / datarev / shape / bytes / mtime /
fsize / name / member / path` を出すようにした。`src=#K` は実行依存の `srcId`
そのものではなく、同じ source を同じ番号として示す密な通し番号である。

当時の出力契約は次の形だった。

```
srcmap: <tag> uid=… seq=… idx=… src=#K refs=N rev=… datarev=… WxHxC bytes=…
        mtime=… fsize=… name='…' [member='…'] [path='…']
srcmap: <tag> 15 doc(s), 15 source(s), 307200 resident byte(s)
```

大域カウンタである `srcId` の数値は、その frame より先に何枚を復号したかで変わる。
したがって数値自体を表明に使わず、同じ run 内で source が同じかだけを `#K` の一致で
表す、というのが probe の契約だった。

この probe により、機能仕様の D-1（共有）、D-2（Watch 基線）、D-7（rev）の
観測口ができた。ただし、probe だけで当時の stage 2/3/5 の全受入条件が満たせる
わけではない。決め手になるのは、共有 identity、membership close 後の生存、uid
不変、共有先を含む walk など、identity を直接見る項目である。

## 2. 実測結果

| 表明 | 結果 | 実測で確認したこと |
|---|---|---|
| M1 | PASS | stage 1 基線は N docs / N sources / 各1 holder |
| M1 mirror | PASS | per-document mirror と source が一致 |
| M2 | PASS | NPY の `mtime` / `fsize` がディスクの実値と一致 |
| M3 | PASS | RAW の `mtime` / `fsize` がディスクの実値と一致 |
| M4 | PASS | 共有した2 membership が同じ `#K`、`refs=2`、resident は1枚分 |
| M5 | PASS | crop の CoW 後も他方の画素は不変 |
| M6 | PASS | `cloneSource` は内容同一、identity 別、`rev=0` |
| V21 | PASS | NPZ member は container の Watch 基線を持つ |

実測範囲は frame 15/15、RAW 1件、NPZ 5 member。コンテナ経路 `.vnz` の
increment 実行確認はこの run には含まれない。

## 3. CLI とタイムアウト

`--srcmap-selftest` の追加により、当時の CLI は 52→53 flags、selftest flags は
22→23、ctest は22→23となった。

前日の「Browse action phase は無限に待つ」という記録は訂正された。
`waitdir` / `waitimg` は各60秒で終わるが、run 全体の上限がなく、既定列の15 waits
では15分級になり得た。実測では不可能な waits 3個が3分03秒。修正後は listing
成立から300秒で、停止 action と phase、directory、image count を名指しして rc=1
で終了する。

修正後の診断行は次の形だった。

```
browsekeys: action phase gave up after 300 s at action 12/275 'waitimg:44'
(phase 0), dir=…, imgs=…: FAILED
```

健康な run は約35秒、ctest の上限は900秒だった。この修正は、失敗を単なる
`Timeout` にせず、止まった action を証拠として残すためのものだった。

## 4. 当時の受入行への効き方

| 当時の受入行 | probe の役割 |
|---|---|
| 同じ folder を2回開いた source 共有 | `src=#K` 一致が判定になる |
| 共有時の resident bytes | 既存 V14 が主証拠、probe は補助 |
| 片方 close 後の生存 | 統計が主証拠、`refs 2→1` が補助 |
| membership だけ close | source 生存を `refs` で直接確認 |
| compare の same-pixels 文 | この probe ではなく chip 文字列の観測が必要 |
| 既存 derive 表明 | 既存表明が主証拠 |
| derive の resident bytes | 既存会計が主証拠 |
| crop CoW | M4/M5 でこの run 中に実測 |
| reload 後 uid 不変 | probe の `uid` が判定になる |
| reload 後の全キャッシュ再計算 | 鍵の中身は出さないため、この probe だけでは判定できない |
| reload 後の σ_t 更新 | 数値の表明が別に必要 |
| reload 後の fit 破棄 | fit 状態の表明が別に必要 |
| 共有先を含む walk | 全 membership の source/rev で欠落を検出 |
| `temporalExtra` の破棄 | temporal slot の表明が別に必要 |
| 手動 Reload の UI | UI 側の観測が必要 |

`mtime` / `fsize` は NPY と RAW でディスクの `last_write_time` / `file_size` と
突き合わせ、NPZ は V21 で container のサイズと全 member の `fsize` を突き合わせた。
当時未実施だったのは `.vnz` container 経路の increment と、remote の
preview→full 差し替えによる `rev` の +1 である。M6 が確認したのは
`cloneSource` が別 identity を取り `rev=0` になるところまでだった。
