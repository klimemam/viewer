# 2026-08-17 Browse「Open with reader...」UI 検証結果

- 対象コミット: `08e6fb79cb995d5c64101e30b8e2deb230598279`
- 実行環境: Windows / MinGW（実行機の識別情報は記録されていない）
- 種別: Browse 行メニューと Reader panel への操作経路を追加した際の実施記録
- 再実行仕様: [../ui.md](../ui.md) A26
- 記録元: 対象コミットに保存された `docs/verify-ui.md` とコミットメッセージ。今回の文書再構成では再実行していない

## 対象

Browse のファイル行に `Open with reader...` を表示し、実際にクリックすると、その
ファイルの path を持つ Reader panel が開くことを検証した。native で読める `.npy` と、
native では読めないファイルの両方を対象とし、folder / group 行には項目を追加しない。
既存項目の欠落は、メニュー項目数と表示順で検出する。

## fail-first の校正

実装前は、追加した6表明がすべて失敗した。代表的な出力は次のとおり。

```text
chkctx:6 -> 5 item(s): [Open;Open as stack;Open as frame average;Copy path;Properties...;]: FAIL
ctxclick:Open with reader... -> no such item: FAIL
chkrdr:dark.npy -> reader panel CLOSED, path "": FAIL
```

既存の folder / group 行の項目数と `-Open with reader...` の表明は、この時点でも成功して
いた。したがって、追加した観測点は「popup が開いた」だけでなく、欠けている項目と
Reader panel への遷移を区別していた。

## 実測結果

| 項目 | 実測 | 判定 |
|---|---|---|
| native file 行の全項目 | `chkctx:6 -> 6 item(s): [Open;Open as stack;Open as frame average;Open with reader...;Copy path;Properties...;]: ok` | PASS |
| folder 行の項目数と非表示 | `chkctx:4 -> 4 item(s): [Open folder (all stacks below);Search under here;Bookmark;Copy path;]: ok` | PASS |
| Reader panel への実クリック | `chkrdr:dark.npy -> reader panel open on "tools/testdata/rb/dark.npy": ok` | PASS |

対象コミットの記録では、A節は **27項目 / 実行27 / PASS 27 / FAIL 0**、UI
マトリクスのスイート集計は **21 test / PASS 21 / FAIL 0** だった。コミットメッセージの
プロジェクト全体集計は **54/54 selftests pass** である。

この結果は対象コミットに固定し、後日のメニュー項目数や実装状態に合わせて書き換えない。
