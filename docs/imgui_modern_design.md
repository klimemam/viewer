# Dear ImGui モダンデザイン検討 — "Aurora" テーマ

Dear ImGui を「開発のテンションが上がる & 情報が分かりやすい」見た目にするための
デザイン検討です。**動くコード**(`imgui_theme.py` + `imgui_demo.py`)と、
このドキュメントの2点セットで構成しています。

```
pip install -r requirements-imgui.txt
python imgui_demo.py
```

デモは本リポジトリの Viewer(PySide6 版)と同じ画面構成
(ギャラリー | ビューア | 統計、ツールバー、ステータスバー)を
Dear ImGui + docking で再現したモックです。画像・サムネイル・ヒストグラムは
すべて手続き生成なので、アセットなしでどこでも起動します。

## スクリーンショット

**ダーク(基本形)**

![dark](img/imgui_dark.png)

**ライト**

![light](img/imgui_light.png)

**A/B 比較モード**

![compare](img/imgui_compare.png)

## デザインコンセプト

> 「静かなグレー + アクセント1色」— 主役は常に画像。UI は一歩引く。

ImGui デフォルトテーマからの脱却ポイントは4つだけです。逆に言うと、
この4点を押さえるだけで一気に"今どき"の見た目になります。

1. **レイヤー化した背景色** — 真っ黒/真っ白を使わず、
   「アプリ背景 → パネル → 入力部品」の3段階の明度差で奥行きを作る。
   さらに僅かに青方向へシフトさせ、"設計されたグレー"にする。
2. **アクセントは1色だけ** — 選択・チェック・スライダー・アクティブタブの
   オーバーラインなど「今どこ/何が選ばれているか」にのみ使う。
   ボタンはニュートラル。色数を絞ることが分かりやすさに直結する。
3. **角丸と余白** — `FrameRounding 5` / `WindowRounding 8` と、
   デフォルトより一回り広い `FramePadding` / `ItemSpacing`。
   ImGui が窮屈に見える最大の原因は余白不足。
4. **境界線はヘアライン** — 不透明な枠線をやめ、
   `rgba(255,255,255,0.06)`(ライトでは黒の10%)の極薄ラインに。
   区切りは「線」ではなく「明度差」で見せる。

## カラーパレット(ダーク)

| 役割 | 値 (sRGB) | 用途 |
| --- | --- | --- |
| bg0 | `#131318` | ドックスペース / アプリ背景 |
| bg1 | `#19191F` | ウィンドウ、メニューバー |
| bg2 | `#23232C` | 入力部品、ボタン、テーブルヘッダ |
| text | `#E8E8F0` | 本文 |
| text dim | `#8A8A99` | 補助情報(ファイル名、ラベル) |
| hairline | `#FFFFFF` @6% | 境界線、セパレータ |
| accent | `#6B8CFF`(Aurora Blue) | 選択・フォーカスの表示のみ |

アクセントはプリセット5色(`imgui_theme.ACCENTS`)から選択でき、
カスタム色も指定可能です。ライトバリアントでは同じアクセントを
自動で 18% 暗くしてコントラストを確保します。

## 形状・余白の設計値

| パラメータ | 値 | メモ |
| --- | --- | --- |
| WindowRounding / PopupRounding | 8 px | パネルは大きめの角丸 |
| FrameRounding / GrabRounding | 5 px | 部品は控えめ |
| TabRounding | 6 px | |
| FramePadding | 10 × 6 | デフォルト(4×3)の約2倍 |
| ItemSpacing | 10 × 8 | |
| WindowPadding | 14 × 12 | |
| ScrollbarSize | 12 px | 背景透過 + 丸グラブで存在感を消す |
| Border 全般 | 1 px ヘアライン | 色側で薄くする(形状は 1px のまま) |

タブは「選択タブの上辺にアクセントのオーバーライン」
(`ImGuiCol_TabSelectedOverline`)を使うのがポイントで、
ブラウザ風の"今ここ"表示になります(ImGui 1.90.9+)。

## 分かりやすさのための UI ルール

- **選択サムネイルはアクセント枠 2.5px + ファイル名を明色に**。
  非選択のファイル名は text dim。一覧の中で現在地が一目で分かる。
- **ピン留め(A)は `[A]` プレフィックス + ビューア側ヘッダをアクセント色に**。
  比較モードで「どちらが基準か」を迷わない。
- **ステータスバーは左=位置(n/N とファイル名)、右=状態(解像度・ズーム)**
  という固定レイアウト。視線移動が安定する。
- ヒストグラムの RGB は意味色(赤/緑/青)で固定し、テーマの影響を受けない。

## 実装メモ

- `imgui_theme.py` は**素の ImGui スタイル値を設定するだけ**なので、
  pyimgui / imgui-bundle / C++ 本家のどれにも同じ数値で移植できます。
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

## フォントについて(今後の課題)

hello_imgui 同梱フォント(DroidSans + FontAwesome)でも十分見られますが、
日本語 UI にするなら **Noto Sans JP** / **IBM Plex Sans JP** を
`hello_imgui.load_font("NotoSansJP-Regular.ttf", 16, japanese_glyphs=True)`
相当で読み込むのが次の一手です(タブ・メニューの日本語化とセット)。

## PySide6 版との関係

現行の `main.py`(PySide6)を置き換えるものではありません。
このスタディは「もし Dear ImGui でビルドし直すならこう見える」を
確かめるためのもので、採用する場合は immvision のズーム/パン機能を
活かして `image_view.py` 相当を置き換えるのが最短ルートです。
まずはデモを触ってテンションを確かめてください。
