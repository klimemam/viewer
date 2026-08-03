現行ドキュメント: [imgui_modern_design.md](../imgui_modern_design.md) の背景 — 設計値の出自になった Python のデザイン実験場(2026-08-03 に削除)。

# Dear ImGui モダンデザイン検討 — "Aurora" テーマ — 経緯と検討

> テーマの出自は Python のデザイン実験場 (`imgui_theme.py` / `imgui_demo.py`) で、
> 値が C++ に移った時点でそちらは役目を終えたため 2026-08-03 に削除しました。
> 以下の Python 版に関する記述は、設計値の由来としてそのまま残しています。

## Python デモ(デザイン実験場)

```
pip install -r requirements-imgui.txt
python imgui_demo.py
```

ギャラリー | ビューア | 統計というレイアウトのモックで、Design Lab パネルから
ダーク/ライト・アクセント色・角丸を**その場で**動かせます。C++ に反映する前の
色検討はここでやるのが最速です。画像・サムネイル・ヒストグラムは
すべて手続き生成なので、アセットなしでどこでも起動します。

**ダーク**

![dark](../img/imgui_dark.png)

**ライト**

![light](../img/imgui_light.png)

**A/B 比較モード**

![compare](../img/imgui_compare.png)

## 実装メモ

- C++ 版は
  [core/ui_theme.cpp](../../core/ui_theme.cpp)、Python 版は `imgui_theme.py` で、
  両者は同じ数値を共有しています(片方を変えたらもう片方へ同期)。
- デモは [imgui-bundle](https://github.com/pthom/imgui_bundle) の
  hello_imgui(docking レイアウト)+ immvision(numpy 画像表示)+
  implot(ヒストグラム)を使用。
- hello_imgui は ini に保存したテーマを起動後に再適用するため、
  デモでは `remember_theme = False` にした上で毎フレーム
  `pre_new_frame` からテーマを適用しています(スタイル値の代入のみで軽量)。
- Design Lab パネルで ダーク/ライト・アクセント色・角丸を**実行中に変更**
  できます。デザイン調整の試行錯誤はここで行うのが速いです。
- スクリーンショットは headless で再生成できます:
  `xvfb-run python imgui_demo.py --screenshot docs/img/imgui_dark.png [--light|--compare]`

## フォントについて

デモは同梱の **Roboto Regular 16px** + FontAwesome(アイコン)を使用しています。
ImGui 1.92 以降はグリフを動的に読み込むため、グリフ範囲の指定は不要です。
日本語 UI にする場合は **Noto Sans JP** / **IBM Plex Sans JP** の ttf を
リポジトリに追加し、`_load_fonts()` の1行を差し替えるだけで済みます。
