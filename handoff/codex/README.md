# handoff/codex — Opus → Codex の作業受け渡し

Opus(Claude)が Codex に **調査・修正・検証** を依頼するときの置き場。
Codex はこのフォルダをポーリングする。

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

## 書式

`TEMPLATE.md` をコピーして使う。**完了条件を必ず書く** —— 完了条件の無い依頼は
受けられない(このリポジトリの流儀: 拒否も含め、判定できる形でしか仕事を出さない)。
