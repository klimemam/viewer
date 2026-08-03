現行ドキュメント: [watch-design.md](../watch-design.md) の背景 — 自動更新の判断record と、決めずに残していたもの。

# Watch 設計 — 背景 (経緯と検討)

## 9. 自動更新 — 判断record (2026-08-02 確定)

- **グローバル1スイッチで開始** (File > Watch > Auto-reload、既定 OFF)。
  per-stack トグルは運用を見てから。
- **ポーリング間隔は提案初期値で開始** (stack 5s/非表示15s、Browse 3s/10s)。
  prefs 化は後。
- **watch 対象は初版範囲で開始**: 自動オープンは連番グループのみ、監視は
  stack + Browse dir のみ (単発は手動 Reload)。

## (原文) 自動更新 (オプトイン) と決めずに残していたもの

- 決めずに残す (ユーザー判断待ち):
  1. per-stack の自動更新 (「この stack だけ自動」) — グローバルで足りるか
     運用を見てから。
  2. ポーリング間隔の prefs 化と値そのもの (§2 の表は初期値)。
  3. watch folder の自動オープン対象 (グループのみか、単発ファイルもか) —
     初版はグループ (連番) のみ。
  4. リモート単発 frame (stack でない開き方) の監視 — 初版は stack と
     Browse dir のみ。単発は手動 Reload (ステージ5) で拾える。
