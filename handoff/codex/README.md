# handoff/codex — Opus → Codex の作業受け渡し

Opus(Claude)が Codex に **調査・修正・検証** を依頼するときの置き場。
Codex はこのフォルダをポーリングする。

## 作業場所の取り決め(2026-08-17 合意)

1. **主 checkout(このフォルダ直下)は main のまま触らない** —— ブランチを切らない・
   ステージを残さない。Opus が board/docs を main に直接コミットする場所で、ここの
   HEAD/index が動くと「別ブランチに他人のコミットが積まれ、ステージ残りが他人の
   メッセージでコミットされる」事故が起きる(2026-08-17 に実際に起きた)
2. **作業は worktree で**: 名前は `viewer_work/codex-<内容>`、基点は **`origin/main`
   を明示**(ローカル main は遅れていることがある)
   ```
   git fetch origin && git worktree add viewer_work/codex-<slug> -b <branch> origin/main
   ```
3. **ビルドは worktree 内に自前の build-mingw**。deps は
   `-DFETCHCONTENT_SOURCE_DIR_*` で主 checkout の `build-mingw/_deps` を指してよい。
   selftest は per-test TMP/config-dir 化済みなので並行実行して安全
4. **着手前に名乗る**: このフォルダの依頼なら該当ファイルを `status: taken` に。
   自発タスクなら盤(docs/tasks.csv)の行番号を issue で名乗る。Opus 側のエージェントと
   同じ領域を同時に触らないための唯一の仕組みなので省略しない

## 契約

- **1依頼 = 1ファイル**。名前は `YYYYMMDD-NN-slug.md`(NN はその日の連番)
- 依頼の**判断**はここに書かない —— 判断は issue、正典は `docs/tasks.csv`。
  ここにあるのは作業の受け渡しだけで、必ず issue 番号を参照する
- このフォルダと自分の作業ブランチ以外を Codex は触らない

## ポーリング手順(Codex 側)

1. `git pull`
2. frontmatter が `status: open` のファイルを探す(`grep -l "status: open" handoff/codex/*.md`)
3. 着手時: `status: taken` に書き換えて commit(名乗り。二重着手の防止)
4. 完了時: **結果** 節に記入して `status: done` に書き換えて commit

## 種別ごとの届け方

| kind | 成果物の置き場 |
|---|---|
| 調査 | このファイルの **結果** 節に追記して main へ commit |
| 検証 | 同上(実行環境・手順・判定を書く。過去の実行結果は書き換えない) |
| 修正 | ブランチ + PR(PR 本文の1行目に handoff id を書く)。結果節には PR 番号だけ |

## 状態

`open`(未着手)→ `taken`(Codex 作業中)→ `done`(結果記入済)。
done の回収(issue / 盤への反映)は Opus がやる。ファイルは履歴として残す。

**逆向き(Codex → Opus)**: 作業中に Opus の判断・情報が要るときは、同じファイルの
**質問** 節に書いて `status: question` にして commit + push する。Opus はこのフォルダを
監視しており、答えを **回答** 節に書いて `status: taken` に戻す。依頼と無関係な
新規の相談は `q-YYYYMMDD-NN-slug.md`(kind: 相談)を新規に作ってよい —— 書式は同じ。

## 署名

このリポジトリは1つの GitHub アカウントを3者(ユーザー / Opus / Codex)で共有している。
issue・PR のコメントは**必ず1行目に署名**を置く: Opus は `🤖 **Opus**`、Codex は
`🤖 **Codex**`。handoff ファイル内は書いた節が明らかなので署名不要。

## 会話の使い分け

| 用途 | 場所 |
|---|---|
| 作業の受け渡し・作業中の質疑 | このフォルダ(status で状態を運ぶ) |
| 判断・裁定・レビュー | issue / PR のコメント(署名つき。ユーザーも読む場所) |
| 状態の正典 | `docs/tasks.csv`(会話をここに書かない) |

## 書式

`TEMPLATE.md` をコピーして使う。**完了条件を必ず書く** —— 完了条件の無い依頼は
受けられない(このリポジトリの流儀: 拒否も含め、判定できる形でしか仕事を出さない)。
