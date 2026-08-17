# handoff/codex — Opus → Codex の作業受け渡し

Opus（Claude）がCodexに**調査・修正・検証**を依頼するときの置き場。

Codexはこのフォルダを自動監視しない。ユーザーまたはOpusが会話でhandoff IDか
ファイルを指定したときに着手する。

## 正典

- 判断と議論はGitHub Issue、課題状態は`docs/tasks.csv`を正典とする。
- handoffファイルには依頼、参照先、完了条件、結果だけを書く。
- 1依頼につき1ファイルとし、必ずIssue番号と判定可能な完了条件を書く。
- ファイル名は`YYYYMMDD-NN-slug.md`とする。`NN`はその日の連番。

## 共有checkoutを守る

主checkoutは、Opusの作業場所として`main`のまま残す。Codexは主checkoutの
HEAD、index、作業ファイルを変更しない。作業には、更新した`origin/main`を基点とする
専用worktreeを使う。

```text
git fetch origin
git worktree add viewer_work/codex-<slug> -b <branch> origin/main
```

- ビルド出力はworktree内に置く。
- 他のworktreeや他者の未コミット変更を操作しない。

## 作業手順

1. handoffファイル、Issue、参照文書を読む。
2. Issueに同じ作業の着手宣言がないことを確認する。同時作業の可能性がある場合は、
   Issueにhandoff IDとブランチ名を書いてから着手する。
3. 専用worktreeで調査、修正、検証を行う。
4. handoffファイルの**結果**節を記入し、`status: done`へ変更する。
5. 成果物はPRで提出し、本文の冒頭にhandoff IDを書く。mainへ直接コミットしない。

## 状態

`open`（未着手）→ `done`（結果記入済み）。`status: done`への変更は成果物PRに
同乗させる（mainへ直接コミットしない、の帰結）。

作業の衝突防止にはIssueの着手宣言を使う。**双方の義務**: Codexは作業手順2のとおり、
OpusもIssueに紐づく作業をエージェントに出すときは同じ着手宣言（ブランチ名つき）を
Issueに書く。片側だけの排他は排他ではない。Issueや`docs/tasks.csv`への反映は、
依頼に含まれる場合だけCodexが行い、それ以外はOpusが行う。

## 署名

このリポジトリは1つのGitHubアカウントを3者（ユーザー / Opus / Codex）で共有している。
Issue・PRのコメントは**必ず1行目に署名**を置く: Opusは`🤖 **Opus**`、Codexは
`🤖 **Codex**`。着手宣言も署名がなければ誰の宣言か分からない。

## 会話の使い分け（2026-08-17 合意、PR #220）

| 用途 | 場所 |
|---|---|
| 作業の受け渡し | このフォルダ（依頼・参照・完了条件・結果のみ） |
| **作業中の質疑** | **参照Issueへの署名コメント**（Codexはフォルダを監視しないため、ファイルstatusではなくIssueが質疑の線。OpusはPRとIssueを監視している） |
| 判断・裁定・レビュー | Issue / PRのコメント（署名つき。ユーザーも読む場所） |
| 状態の正典 | `docs/tasks.csv`（会話をここに書かない） |

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

`TEMPLATE.md`をコピーして使う。完了条件のない依頼には着手しない。
