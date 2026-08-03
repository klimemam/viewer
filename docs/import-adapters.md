# 読めない形式をどう読むか — native の範囲と入力アダプタ

native が読む形と、それ以外を読むための入力アダプタの契約。

---

## 3. native の範囲 — 決め打ちにする

### 3.1 native が読む形 (`.npy` / `.npz` メンバ / raw)

**判定は次元数と、最後の軸だけ**を見る。**先頭の軸の大きさは見ない** —
「先頭が4以下ならチャンネル」が、3枚の stack と 3ch の画像を取り違える元だった。

| shape | 読み方 |
|---|---|
| `(H,W)` | 1枚・1ch (frame) |
| `(H,W,3)` / `(H,W,4)` | **1枚のカラー** (RGB / RGBA)。最後の軸が 3 か 4 のときだけ |
| `(F,H,W)` | **F 枚の stack**・各1ch (`(3,H,W)` も3枚) |
| `(F,H,W,C)` | F 枚 × C ch |
| 上記以外 (1次元・5次元以上・最後の軸が 2 や 5 の3次元) | **読まない。** adapter を促す |

**例外は1つだけ、明文化されている**: 3次元で最後の軸が 3 か 4 ならカラー。
それ以外の3次元は stack。これで実用上のカラー画像は読めたまま、
**`(3,H,W)` は3枚の stack** になる (以前は3chカラーと誤読されていた)。

残る取り違えは `(5,8,3)` のような「幅3ピクセルの5枚 stack」だけ。
現実には無く、あっても §3.3 の言い直しで直せる。

### 3.2 読まなかったときに何を言うか

黙って諦めない。**形と、次にすることを言う**:

```
odd.npy: shape (4, 8, 8, 8, 2) is not a native form
  native reads (H,W) / (H,W,3|4) / (F,H,W) / (F,H,W,C)
  [ choose a reader... ]        ← その場から adapter を指定できる
```

### 3.3 取り違えたときの逃げ道 — Inspector で言い直す

Inspector に「どう読んだか」を出し、**その場で言い直せる**:

```
read as   1 frame x 3 ch   (H,W,C)         [ re-read as... ]
                                            ├ 480 frames x 1 ch  (F,H,W)
                                            └ 1 frame x 1 ch, first plane only
```

- **選択肢はその配列の形から計算される**。ありえない読み方は出さない
- 言い直すと**同じファイルを読み直す** (path は FrameSource が持っている)。
  推測ではなく**ユーザーの宣言**なので、以後その doc はその読み方で扱われる
- セッションに保存され、次に開いても同じ読み方になる
- **失うものは明示する**: 読み直しは doc を作り直すので、その doc に紐づく
  比較スロットのピンや、フレーム番号に依存する状態は張り直しになる。
  何が失われるかを実行前に言う

### 3.4 これで消えるもの

- **`--npy-axis`** — 3次元の意味を人が上書きするための**グローバルな旗**。
  同じ実行の中に「stack として読みたいファイル」と「カラーとして読みたい
  ファイル」が混在していても、旗は1つしかなかった。
  §3.3 の言い直しは**ファイルごと**で、しかも画面に見えている
- `decodeNpyBuffer` の `firstIsChannels` 分岐と、暗黙の `CHW->HWC` 変換
  (`(3,H,W)` は3枚になる。CHW のカラーが欲しければ §3.3 で言い直す)

### 3.5 native のままにするもの

`.npy` / `.npz` / raw の**形式**そのものは native のまま。日常の9割で外部プロセスを
挟む理由がない。狭めたのは「どの *形* を受けるか」であって、扱う形式ではない。
raw (ヘッダ無し) は元々ユーザーが dtype・寸法・解釈を明示するので、
そもそも推測が無い — 今のままでよい。

---

## 4. 入力アダプタの仕様

### 4.1 契約はユーザーが書く「関数」

**契約は関数の返り値**。

```python
def load(path):            # path: str。この1引数だけが必須
    ...
```

- 関数名は自由。viewer には `adapters/acme.py:load` の形で指定する
  (1ファイルに複数の reader を置ける)
- **`path` はフォルダのこともある** (2026-08-03 決定)。series は「1条件1フォルダ」
  「1条件1ファイルのフォルダ」で保存されることが多く、**フォルダを1つの
  `Series` にする**のが自然な単位。adapter 側で `os.path.isdir` を見て分ければよく、
  API は増やさない。viewer 側は Browse のフォルダ行と picker から reader を指定できる
- モジュールは**自分のディレクトリを sys.path に載せて**実行される
- **元ファイルを書き換えてはならない。** 読むだけ
- **将来のための予約** (今決めておけば後で破壊的変更にならない):
  関数が `only` キーワード引数を受けるなら、viewer は「この範囲だけ欲しい」と
  渡すことがある。受けない関数はこれまでどおり全部返す。

  ```python
  def load(path, only=None):   # only: range | list[int] | None (全部)
  ```

  **adapter が自分の判断で一部だけ返すときは、その事実を `note` に残すこと。**

### 4.2 返り値が層を名乗る

**返り値の型が、frame / stack / series / batch のどれかを名乗る。**
層モデルの4語以外の語を持ち込まない (`frames` のような新語を作らない)。

```python
from viewer_import import Frame, Stack, Series, Batch

return Frame(img)                       # 1枚
return Stack(arr)                       # 複数フレーム、同一条件
return Series(stacks, conditions=...)   # 条件を振ったもの
return Batch([dark, flat])              # 一緒に開くもの
```

**返し方は2通りだけ。** 素の配列か、上の型か。

```python
def load(path):
    return np.load(path)["data"]        # 1. 素の配列: native と同じ読み方。1行のまま
```

素の配列は §3.1 の native と同じ規則で読む (覚えることを増やさない)。
**曖昧さを自分で潰したくなった時点で型を名乗る** — それが2つ目の道。

### 4.3 層ごとのフィールド

| 層 | 位置引数 | 追加のフィールド |
|---|---|---|
| `Frame` | 画素 | `cfa` `name` `note` `meta` `range` |
| `Stack` | 画素 | `timestamps` `cfa` `name` `note` `meta` `range` |
| `Series` | `Stack` の列 (または画素) | **`conditions` (必須)** `name` `note` `meta` `range` |
| `Batch` | 上記の列 | `name` |

| フィールド | 型 | 意味 |
|---|---|---|
| `timestamps` | `Values` | 各フレームを**いつ撮ったか**。条件は同じまま。§4.3.3 |
| `conditions` | `Values` | stack 間で**変えた条件** (露光・照度)。series の存在理由。§4.3.3 |
| `cfa` | str | `"RGGB"` `"BGGR"` `"GRBG"` `"GBRG"`、quad は `"quad:RGGB"` |
| `name` | str | 表示名。省略時はファイル名 |
| `note` | str | **素性の一文**。画面に出る散文。§4.3.1 |
| `meta` | dict | **撮影条件の事実**。key-value。provenance に載る。§4.3.1 |
| `range` | (lo,hi) または配列 | 表示レンジ。**その層の形に合わせられる**。§4.3.3 |

`timestamps` / `conditions` の値の型はどちらも `Values` 1つ (§4.3.2)。
**値は全桁保持。単位は推測せず、必須** — 空のまま渡された軸は**適用されない**
(貼り付けの規則と同じ)。

綴りを間違えたフィールドは**その場で TypeError になる** — 型を名乗る利点そのもの
(dict なら「知らないキー `foo`」を実行後に報告するしかなかった)。

#### 4.3.1 `note` と `meta` の違い

| | `note` | `meta` |
|---|---|---|
| 何か | **一文の散文**。「これは何で、何が起きたか」 | **key-value の事実**。「どんな条件で撮ったか」 |
| 例 | `"CHW から並べ替えた"` / `"24 中 18 フレーム"` | `{"gain_db": 6.0, "sensor": "IMX999"}` |
| 誰が書くか | adapter **と viewer の両方**。viewer は追記する | adapter だけ。**viewer は運ぶだけで書き換えない** |
| どこに出るか | 画像の隣 (既存の `ImageDoc::note`) | Inspector と**エクスポートの出所行** |
| 増えるか | 合成される (`"frame axis, CHW->HWC"` のように) | key が増えるだけ |

viewer 自身が note を書く場面が既にある — dtype を変換した、n/N しか読めなかった、
並びを直した。**adapter はその同じ一行に足す**のであって、別の場所に書くのではない。

逆に **`meta` に散文を入れない**こと。「暗いので撮り直した」は note であって
meta ではない。meta は後で機械が読む (provenance・将来の series 値)。

**単位が要る量を meta に入れるとき**は値だけ置かない:

```python
"meta": {"gain": {"value": 6.0, "unit": "dB"}, "sensor": "IMX999"}
```

正典の「量には必ず単位」がここにも効く。`"gain_db": 6.0` のように**キー名に
単位を埋めない** — キー名は誰かが選んだ名前であって単位の証明ではない
(単位を key 名から推測しないのと同じ理由)。

#### 4.3.2 `timestamps` と `conditions` — 値の型は1つ

```python
from viewer_import import Stack, Series, Values

Stack(arr,  timestamps=Values(t, unit="s"))                    # 名前は "time" が既定
Series(sts, conditions=Values(exposures, "exposure", "ms"))    # 名前は必須
```

| フィールド | 付く層 | 長さ | `name` | `unit` |
|---|---|---|---|---|
| `timestamps` | `Stack` | **フレーム数** | **省略可** (既定 `"time"`)。`"elapsed"` 等で上書き可 | **必須** |
| `conditions` | `Series` | **stack 数** | **必須** — 何を振ったかは viewer には分からない | **必須** |
| `covariates` (将来) | `Stack` | フレーム数 | **キーが名前** | **必須** |

- **型は `Values(values, name="", unit="")` の1つだけ。** 専用型 (`Times` / `Sweep`) は
  作らない — `covariates` を足すときも同じ型で足りる
- **`name` の冗長は、フィールド側が既定を持つことで消す。** `timestamps` に
  `"time"` と書き足す必要はない (書きたければ書ける)
- **`unit` は常に必須。** 時刻ですら `"s"` か `"ms"` かは推測しない。
  空なら**その軸は適用されない** — 貼り付けの規則と同じで、ファイル由来だからと
  緩めない
- **複数形は「メンバ1つにつき1つ」を語形が言う。** 長さの規則が名前から読める

将来 `covariates` を足すときの形 (v1 では受けない、名前だけ予約):

```python
Stack(arr, timestamps=Values(t, unit="s"),
           covariates={"temperature": Values(temp, unit="C")})
```

#### 4.3.3 `range` は、それが属する層の形に合わせられる

| 書き方 | 形 | 意味 |
|---|---|---|
| `range=(lo, hi)` | 2 | その層の全部に同じレンジ |
| `Stack` に `range=arr` | `(F,2)` | **フレームごと** |
| `Series` に `range=arr` | `(S,2)` | **stack ごと** |
| `Series` に `range=arr` | `(S,R,2)` | stack ごと × フレームごと |

長さが合わなければ**拒否** (`conditions` と同じ規則)。

**ただし警告を1つ**: フレームごとに違うレンジを与えると、**並べても目で比較
できなくなる**。同じ明るさに見える2枚が、実は違う伸ばし方をされている。
viewer には既に同じ性質の機能があり (`Auto per frame`、montage の per-frame
range は「values are NOT DN, view only」と明記している)、それと同じ扱いにする:

- レンジが一様でない stack / series は、**そうであることを画面が言う**
  (note に `display range varies per frame`)
- **測定値には一切触らない**。σ_t も平均も生の値のまま。range は表示だけ

### 4.4 形の判定は、名乗った層の下で行う

**`layout` を書かなくてよくする**のがこの設計の要点。層が分かっていれば、
同じ shape でも意味が一意に決まる:

| 層 | 受ける shape | 意味 |
|---|---|---|
| `Frame` | `(H,W)` | 1ch |
| `Frame` | `(H,W,C)` | C ch。**C の値は問わない** (frame に frame 軸は無いので曖昧さが無い) |
| `Stack` | `(F,H,W)` | F 枚・1ch。`(3,H,W)` は3枚 |
| `Stack` | `(F,H,W,C)` | F 枚 × C ch |
| `Series` | `Stack` の列 | そのまま。`conditions` の長さ == 列の長さ |
| `Series` | `(S,H,W[,C])` | S 条件 × 各1枚 |
| `Series` | `(S,R,H,W[,C])` | S 条件 × R 繰り返し |
| どれでも | 合わない shape | **拒否**。層と shape を両方名指しして言う |

つまり **`(3,H,W)` を3枚にしたいなら `Stack(arr)`、3ch のカラーにしたいなら
`Frame(arr)`** と書く。`layout` も `--npy-axis` も要らない。

`layout` は残すが**転置された並びのためだけ**の逃げ道 (`"CHW"` `"FCHW"`)。
普段は書かない。

### 4.5 `timestamps` と `conditions` の違い — 「経っただけ」か「設定を変えた」か

一文で言うと:

> **オペレータが設定を変えたなら `conditions`。ただ時間が経っただけなら `timestamps`。**

| | `timestamps` (Stack につく) | `conditions` (Series につく) |
|---|---|---|
| 何を表すか | 各フレームを**いつ撮ったか** | stack 間で**変えた条件** |
| 例 | 時刻 [s]、開始からの経過 [s] | 露光 [ms]、照度 [lx]、ゲイン [dB] |
| 撮影条件は | **同じ** | **各点で違う** |
| σ_t | **意味を持つ** (繰り返しなので) | **条件をまたぐ計算は拒否** |
| 何が描けるか | ドリフト (時間に対する平均値の変化) | リニアリティ・PTC |
| 層 | stack の中の座標 | series を series たらしめるもの |

判断に迷う例:

- **温度が勝手に上がった連続撮影** → `timestamps` (時刻)。設定は変えていない。
  温度そのものは観測された共変量で、v1 では受けない (将来 `covariates`)
- **温度を段階的に設定した測定** → `conditions`。各点が別の条件
- **撮影時刻を記録した露光掃引** → `Series` + `conditions`(露光)。時刻も残したければ
  各 `Stack` の `timestamps` に入れる (両方持てる)

### 4.6 層の宣言が、そのまま σ_t の可否になる

**同じ `(F,H,W)` が2つの違うものを表しうる**:

- **同一条件の F 回の繰り返し** → `Stack`。σ_t (時間ノイズ) が意味を持つ
- **条件を振った F 枚** (露光 1,2,5,10 ms) → `Series`。ここで σ_t を計算すると
  **露光の掃引を「時間ノイズ」として報告する**ことになる

正典が「σ_t は stack の性質」と言っているのはこのため。形からは決まらない。
**型がその答えそのもの**なので、書く人が意識するのは「繰り返しか、振ったか」の1点:

| 書くもの | できる測定 |
|---|---|
| `Stack(arr)` | σ_t、σ_s、欠陥画素 |
| `Stack(arr, timestamps=...)` | 上 + 時間に対するドリフトの図 (条件は同じまま) |
| `Series(..., conditions=...)` | リニアリティ・PTC。**条件をまたぐ σ_t は拒否** |

- `Series` の各 `Stack` が繰り返しを持てる (EMVA 的な撮り方はこれ)
- series メンバシップは正典どおり明示的に作られ、**後から自動で継がれない**
- `conditions` の長さが合わなければ**拒否** (黙って辻褄を合わせない)

素の配列を返したときは `Stack` (繰り返し) として扱われる。それが違えば σ_t は
嘘になるので、note に `repeats (returned as a bare array)` を残す —
数値の出所が後から辿れること。

### 4.7 よくある保存形の書き方

```python
# 1. 同一条件で 24 枚
return Stack(arr)                                  # (24, H, W)

# 2. 露光 8 通り x 各 16 枚
return Series(arr, conditions=Values(exposures, "exposure", "ms"))   # (8, 16, H, W)

# 3. 露光 8 通り x 各 1 枚
return Series(arr, conditions=Values(exposures, "exposure", "ms"))   # (8, H, W)

# 4. 同一条件の 24 枚 + 撮影時刻 (σ_t は有効なまま)
return Stack(arr, timestamps=Values(t, unit="s"))  # (24, H, W)

# 5. カラー1枚
return Frame(img)                                  # (H, W, 3)

# 6. 3枚の stack (以前は 3ch と誤読された形)
return Stack(arr)                                  # (3, H, W)

# 7. 別々に撮った dark と flat
return Batch([Stack(d, name="dark"), Stack(f, name="flat")])
```

### 4.8 dtype

- 配列の dtype がそのまま「このデータの型」として表示される (`u8/u16/i32/f32/f64`)
- viewer 内部は f32。**整数は値を保ったまま f32 に載る** (画素値は [DN])
- `bfloat16` など numpy に無いものは f32 に変換し、**変換したと note に残す**
- 非数値 (文字列・object・complex) は拒否。理由を言う

### 4.9 失敗のしかた

例外を投げる。メッセージがそのままユーザーに出る。

```python
raise ValueError("header says 12 planes but the file holds 9")
```

**黙って空を返さない。**「開けませんでした」だけの表示にはしない — 理由が要る。

### 4.10 型の検査と `viewer_import.py` の約束

構築時に検査するもの: 名乗った層に対して shape が合っているか (§4.4) /
`conditions` 長 == stack 数 / `timestamps` 長 == frame 数 / `range` の形 (§4.3.3) /
`cfa` が既知のパターンか / 非数値 dtype の拒否。

`viewer_import.py` の約束:

- **単一ファイル・標準ライブラリのみ**。numpy すら import しない
  (配列は触らず持ち回るだけ)。**コピーして持っていける**こと
- torch も import しない。`__array__` / `.numpy()` を duck typing で見るだけ
- dataclass は `frozen=True`。返したあとに誰も書き換えない
- **import できない環境でも素の配列は返せる** — これが2通りにしておく実利。
  型は表現力のために足すのであって、依存を増やすためではない

### 4.11 harness が引き受けること (ユーザーが書かなくてよいこと)

- torch → numpy (`.detach().cpu().numpy()` 相当、duck typing)
- 非連続配列の連続化、endianness の正規化
- 返り値の検査 (形・dtype・`conditions`/`timestamps` の長さの整合)
- **.npz への書き出し**。予約メンバ:
  `__pixels_<i>` / `__conditions_values_<i>` / `__conditions_name_<i>` /
  `__conditions_unit_<i>` / `__timestamps_values_<i>` / `__timestamps_name_<i>` /
  `__timestamps_unit_<i>` /
  `__cfa_<i>` / `__name_<i>` / `__note_<i>` / `__layout_<i>` / `__meta_<k>`
- 1行の要約を stderr に出す (viewer がそのまま中継する)

**npz を運び屋にするのは、viewer が既に読める形式だから** — 転送のために
新しいパーサを増やさない。これは実装中の「key 付き npz」対応
(docs/npz-design.md) と同じ土台に乗る。二つは競合ではなく、後者が前者を運ぶ。

### 4.12 パスと関数の対応を憶える

一度指定したら憶える。ただし**1ファイル1エントリでは増える一方**なので、
記憶するのは「規則」:

| 記憶する形 | 例 |
|---|---|
| ディレクトリ + 拡張子 | `//nas/share/2026/** : *.npz` → `adapters/acme.py:load` |
| glob | `*_raw.bin` → `adapters/acme.py:load_raw` |
| 単一ファイル (最後の手段) | `.../odd_one.dat` → … |

- **より具体的な規則が勝つ**
- **有限に抑える**: 単一ファイル規則だけ LRU (既定 64 件)。規則は残る
- **prefs に保存し、一覧で見えて消せる**。見えない魔法にしない —
  後から「なぜこのファイルだけ違う読まれ方をするのか」が説明できること

**キャッシュの鍵**: `(元ファイルの path, mtime, size, adapter ファイルの path,
その mtime, 関数名, モジュールの VERSION 属性があればその値)`。
adapter を編集したら次に開いたときに効く。

### 4.13 どこで指定するか / どこで書くか / 信頼

- **Inspector に「reader」欄**。今このファイルが何で読まれたかを表示し、
  その場で差し替えられる
- **開けなかったときのエラーからも直接**指定できる (§3.2)。読めない理由を告げた
  同じ場所に「reader を選ぶ…」を置く — 探しに行かせない
- 選択肢は **repo 同梱** (`tools/import/adapters/*.py`) と**ローカル**の両方から
- **新規作成/編集はユーザーのエディタで、v1 から**する (2026-08-03 決定)。
  「New reader...」がテンプレートを書き出して開き、「Edit reader」が今の
  reader を開く。起動は `$EDITOR` → `code -g <file>:<line>` → OS 関連付け の順に試す。
  内蔵エディタは作らない (ImGui でテキストを書かせる仕事ではない)
- **明示的に選んだものだけが走る。** 自動探索も自動適用もしない
  (記憶された規則は「一度ユーザーが選んだ」ことの記録であって発見ではない)
- 起動前に**実際のコマンドを表示**する。Python が無ければその事実を言う

#### 4.13.1 リモートでも v1 から動かす (2026-08-03 決定)

データが向こうにあるのに adapter を手元で走らせると、**生ファイルを転送してから
変換する**ことになり、この道具の設計思想 (見えているものと数値だけが流れる) に
反する。したがって **adapter は peer 側で走る**。

- adapter は**テキストなので protocol で送れる**。peer は受け取った関数を
  一時ディレクトリで実行し、npz を返す
- peer に Python / numpy が無ければ**そう言う** (推測して黙って失敗しない)。
  そのときの逃げ道は「手元で走らせる (ファイルが流れます)」を明示的に選ばせる
- 選択はローカルで行われたまま。**peer が勝手に adapter を探すことはない**

---

## 6. コスト・危険

- **信頼**: 外部プロセスを起動する。明示設定のみ、実行前に何を起動するか表示する
- **速度**: 起動 + シリアライズのオーバーヘッド。大きな stack はキャッシュが要る
- **Python 依存**: viewer 本体は C++ だけで動く。adapter を使う人だけが Python を要る
- **読めるものが狭くなる**: §3.4 の移行。**これは意図した代償** —
  黙って間違えるより、読まずに理由を言うほうがよい
- **嘘の宣言は検証できない**: adapter が「これは 8 条件だ」と言えば viewer は信じる。
  検証できるのは形の整合だけ (長さ・次元・dtype)。それは harness が必ず行う

## 7. 段階

| # | 内容 | 検証 |
|---|---|---|
| 0 | **native を §3.1 に絞る** + 読まないときの言い方 (§3.2) + HWC 猶予 | 既存 selftest が緑のまま / `(H,W,3)` が note つきで開くこと / 1次元と5次元が理由つきで拒否されること |
| 1 | 返り値の契約 (§4.1-4.8) + `tools/import/viewer_import.py` (型3段) | Python 側だけで完結。3段が同じ結果を出すこと |
| 2 | harness `run_adapter.py` + 同梱 adapter (key 付き npz / HWC) + テンプレート | 手で叩いて npz が出ること |
| 3 | viewer 側: 起動・npz 受け取り・Inspector の reader 欄 | 新 selftest: 偽 adapter を起動し、層と軸ができること |
| 4 | パス→関数の規則 (prefs 保存・一覧・LRU) | 具体性順に勝つこと、LRU が上限を守ること |
| 5 | キャッシュ (元ファイル + adapter の mtime で無効化) | 2回目は起動しない / adapter を編集したら起動する |
| 6 | 返り値の検査と断り方 | 嘘の返り値が拒否され、理由が言われること |
| 7 | peer 側での実行 (リモート) | 既存の remote selftest に1本足す |

経緯と検討: [.background/import-adapters.md](.background/import-adapters.md)
