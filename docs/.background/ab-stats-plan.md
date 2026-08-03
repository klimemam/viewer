現行ドキュメント: [ab-stats-plan.md](../ab-stats-plan.md) の背景 — 着手前の状態、フェーズ分けとリスク。

# A/B 統計パネル — 経緯と検討

元の表題は「A/B 統計パネル計画 — compare 中に B の統計も出す」。着手前の状態はこうだった:

現状、5 つの統計パネルは `cur()` だけを見てキャッシュも 1 枚分しか持たない。本書は「重ねる」と
「画像と同じ並びで横に並べる」の**両方**を入れるための仕様。

## 7. フェーズ
- **P0 基盤** — キャッシュ `[2]` 化、recompute の引数化、`forgetImage`/`closeAll` の両スロット、
  `app.abStatsLayout`、`drawABLegend`、`addDashedPolyline`。UI 無変化、bench 不変を確認。
- **P1 Histogram** — 重ね、正規化 y、plane セレクタ、横並び。一番安く一番見られるので最初。
- **P2 Projection** — 重ねの判読性、y レンジ共有、長さ不一致の退避、横並び。
- **P3 Temporal** — `A|B|Δ|Δ%` 表、B が stack でない場合、server temporal は手動。
- **P4 ROI 表** — 1 ROI = 2 行、clamp 表示、Δ 行トグル。描画コスト再測定。
- **P5 Analysis** — `Run on B` のみ(自動実行なし)、B 専用 provenance。
  各フェーズ末で `--abstats-selftest` に該当 check を追加する。

**リスク**: (1) follow-frame の二重再計算 —— P0 の測定次第で throttle 追加。
(2) CFA 4 系列 × 2 = 8 系列は重ねでは読めず、plane セレクタ(新 UI)が事実上の前提。
(3) 幅の狭いドックでは横並びが成立しない(重ねへ自動退避)。(4) projection の長さ不一致は
**安くは解けない**(正しくやるなら物理座標での位置合わせが要る)。今回は「重ねない + 理由表示」
までとし、揃ったふりはしない。
