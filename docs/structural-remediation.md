# 構造的欠陥の是正方針

対象: [review-new-code.md](review-new-code.md) §2 の P1〜P6。

これは個別の13件を順に塞ぐためのパッチ一覧ではない。同じ種類の欠陥を再び作れない
境界をコードに置き、そこへ段階的に移すための決定である。既存の正しい規約
（1 stack は高々1 series、単位未設定、`rbDefer`、キューへの append、操作中は B の
再計算を保留して stale と表示）は変えない。

## 0. 成功条件

次の6項目を、レビュー時の注意ではなくコード上の不変条件にする。

1. 遅延処理は表示名ではなく、発行元が作った不変 identity で対象を解決する。
2. 遅延処理は実行直前に対象の生存と世代を検証する。
3. 保存・復元で捨てた series/member は、どの分岐でも必ず同じ会計に入る。
4. セッション由来の測定値は、唯一の strict parser を通る。
5. UI の説明と stale 判定は「実際に適用された値」を問い合わせる。
6. ダイアログで得た回答は、それを求めた Open にだけ適用する。

完了とは13症状が消えることだけではない。下記の API を迂回する新しい producer / queue /
session field / range consumer をレビューで機械的に発見できることまでを含む。

## 1. Identity: 表示名を同定に使わない (P1)

### 決定

1回の Open を表す `OpenId` と、そこから作る1 stack を表す `LoadToken` を導入する。
どちらもプロセス内で単調増加し、0 を「古いセッションなど token 無し」に予約する。

```cpp
using OpenId = uint64_t;
using LoadToken = uint64_t;

struct PendingGroup { OpenId openId; LoadToken token; /* display name, files... */ };
struct RemoteOpen  { OpenId openId; LoadToken token; /* display name, url... */ };
struct PendingMember { LoadToken token; /* proposed value... */ };

struct LoadBindings {
    std::unordered_map<LoadToken, int> stackByToken; // token -> seqId
};
```

- Picker が受理された時点で `OpenId` を1個発行し、各 group に異なる `LoadToken` を発行する。
- local / remote の全 stack producer は、`SeqInfo` を作ったその場で `token -> seqId` を記録する。
- pending series は token だけで解決する。名前はラベルであり、F2 rename は同定へ影響しない。
- token の binding は producer の成功時または失敗確定時まで保持し、pending consumer が消費したら
  解放する。Batch を閉じた場合は §2 の一括 cancel が両方を掃除する。
- セッションファイルはプロセスをまたぐので token を永続化しない。復元 identity は
  `normalized path + npzMember + occurrence ordinal within batch` とする。旧形式の path-only は
  fallback とし、曖昧なら先勝ちにせず loss として明示する。

`seqId` はロード後の membership identity、`LoadToken` はロード前後を橋渡しする一時 identity、
表示名は人向け、という3役を混ぜない。

### 禁止事項

- `SeqInfo::name == pending.name` を正しさの条件にしない。
- first-frame path だけを一意キーと仮定しない。
- token がある項目を名前 fallback で救済しない。binding の欠落は producer の不具合として数える。

## 2. Lifetime: 遅延処理を所有者単位にする (P2, P6)

### 決定

キュー項目とダイアログ回答は必ず `OpenId` または安定IDを持つ。対象を破棄する操作は、対象と
その子を参照する全 pending state を単一関数で cancel する。

```cpp
void cancelOpen(OpenId id, CancelReason why);
void cancelBatch(int batchId, CancelReason why);

template<class Id, class Lookup, class Apply>
ApplyResult applyIfAlive(Id id, Lookup lookup, Apply apply);
```

- `closeBatch` は個別 container を直接 erase せず `cancelBatch` を呼ぶ。
- `cancelBatch` は sequence、remote、series pending、raw recipe、token binding を列挙して消し、
  ユーザー操作による cancel と失敗を別の reason で記録する。
- modal の Save、worker completion、pending resolver は実行直前にIDを再検索する。対象が無ければ
  別の対象へ流用せず、create として安全に継続できる操作だけ create へ落とし、それ以外は中止する。
- RAW recipe は `{openId, recipe}` として保持する。先頭キュー項目の `openId` と一致するときだけ
  使用し、Cancel は同じ `openId` の項目だけを消す。
- ID は再利用しない。将来再利用が必要になった場合は `{slot, generation}` に変え、比較に generation
  を含める。

新しい queue/container を追加する変更には、対応する `cancelOpen` / `cancelBatch` の更新と、
「arm → target destruction → fire」のテストを必須とする。

## 3. Loss accounting: 保存・復元を transaction として扱う (P3)

### 決定

session の read / resolve / write は共通の `SessionReport` を受け取り、データを捨てる操作を
素の `continue` や早期 return で表現しない。

```cpp
enum class LossKind {
    MissingTarget, AmbiguousIdentity, DuplicateMembership,
    TruncatedRecord, InvalidValue, PendingNotSerializable
};
struct SessionReport {
    int restoredSeries = 0, restoredMembers = 0;
    std::map<LossKind, int> lost;
};

ResolveResult resolveMember(..., SessionReport& report);
WriteResult writeSessionTo(..., SessionReport& report);
```

- resolve は `Resolved` / `Lost(kind)` を返す。caller が loss を数えるのではなく、捨てる地点で
  report に記録する。
- `made++` は Series が全メンバーの解決を終え、不変条件を満たして commit された後だけ行う。
- 復元は file 全体で使用済み `seqId` を持ち、同じ membership を2回割り当てない。
- `seriesRestore` は保存時にそのまま再出力できるので loss にしない。path をまだ持たない
  picker pending は `PendingNotSerializable` として警告し、保存成功の表示に混ぜない。
- 通常保存は同じディレクトリの一時ファイルへ書き、flush/close 成功後に atomic rename する。
  crash snapshot は atomic rename が使えない失敗も report し、途中の行を正常データと呼ばない。
- 最終 toast は restored と loss の内訳を同時に示す。loss が1件でもあれば clean success の文言を
  使用しない。

## 4. Parsing: セッション境界に唯一の strict parser を置く (P4)

### 決定

測定値は `parseSeriesValue(std::string_view)` のみで読み、返り値を `double` ではなく結果型にする。

```cpp
struct ParsedValue { bool ok; bool unset; double value; };
ParsedValue parseSeriesValue(std::string_view text);
```

- 入力全体を消費する。前後の許可した空白以外が残れば invalid。
- `-` は unset、有限の数だけを value とする。invalid と unset と `0.0` を区別する。
- locale 非依存とし、NaN / infinity / overflow を拒否する。
- `seqlevel`、`seriesmember`、編集 modal、reader/importer が同じ関数を使う。
- 出力は `fmtExact` に統一し、編集欄の初期値にも lossless 表現を使う。

session reader 内の `double` への `operator>>`、`atof`、`strtod` の直接利用は CI の静的checkで
禁止する。新しい測定値fieldには parser のtable-driven testを追加する。

## 5. Effective state: authority を値と説明の両方に使う (P5)

### 決定

range の authority を、black/white だけでなく由来と revision を含む値オブジェクトにする。

```cpp
enum class RangeSource { PerFrame, PerStack, CompareA, CompareB, CompareUnion, Linked };
struct EffectiveRange {
    double black, white;
    RangeSource source;
    uint64_t revision;
};
EffectiveRange effectiveRange(const ImageDoc&, const App&);
```

- Histogram / Projection / texture は同じ `EffectiveRange` を受け取る。
- ラベルは `RangeSource` から作り、`compareRangeMode` から別計算しない。
- cache は `black`, `white`, `revision` を保持し、stale は画像uidだけでなく effective range の
  不一致でも立てる。
- 操作中に再計算を保留する既存方針は維持する。ただし保留した結果を描く全 consumer が
  stale reason (`image changed`, `range changed`) を表示できるようにする。

同じ規則を今後の CFA 解釈、単位、remote/local 状態にも適用する。「画面に何が効いているか」を
mode flag の組合せから consumer が推測せず、authority が effective value と provenance を返す。

## 6. 実装順序

各段階を単独で出荷可能にし、一度に queue/session/range を全面改修しない。

1. **観測可能性:** `SessionReport` と loss 内訳を先に導入し、既存経路を挙動を変えず接続する。
2. **Open scope:** `OpenId` を local/remote/raw queue に通し、cancel を集約する。
3. **Load identity:** `LoadToken` と binding を導入し、producer を一つずつ名前解決から移す。
4. **永続 identity:** session member に `npzMember` と occurrence ordinal を追加し、旧形式 fallback を残す。
5. **Parsing:** 全測定値入力を結果型 parser と `fmtExact` に移す。
6. **Effective range:** 値オブジェクトを導入し、Histogram、Projection、texture の順に移す。
7. **迂回路削除:** name-based pending resolution、process-global raw recipe、consumer 独自ラベル判定を削除する。

段階3が完了するまでは name fallback を残すが、token 付き項目では使わない。段階4のsession keyは
追加のみとし、旧 viewer が未知キーを読み飛ばせる互換規約を守る。

## 7. 検証マトリクス

### Identity / lifetime

- 1ファイル多フレーム `.npy` の各条件から sweep を作り、全 token が異なる seqId に解決する。
- ロード中に stack を別 group と同じ表示名へ変更しても値の所属が変わらない。
- local と remote を混在させても同じ identity 経路を通る。
- modal を開く → pump → 対象を閉じる → Save で、他 series のmembershipが変わらない。
- RAW A/B を連続Openし、recipeとCancelがそれぞれの `openId` に閉じる。

### Session / parser

- 同じfolderを2回開いた同一batch、1つのNPZ内の複数3-D memberを往復する。
- 不正 `seqlevel`、切断した `seriesmember`、重複identityの手書きsessionで loss 内訳を検証する。
- pending restore 中に Ctrl+S 相当を呼び、再保存後もseries blockが残る。
- `0`, `-`, garbage, trailing garbage, overflow, locale小数点、double境界値をtable testする。
- 保存中断を注入し、以前のsessionがatomic renameまで残ることを確認する。

### Effective state

- linked range と A/B 3モードの全組合せで、数値範囲とlabel sourceが一致する。
- Aをstepしてrangeだけを変えた固定Bが `[stale: range changed]` になる。
- move-to-batch後はlinearity fitがinvalidになり、再計算前のrowsを描かない。

テストには `pumpN(n)` と write-failure injection を用意する。happy pathだけを直列に呼ぶテストや、
writerが生成した正常sessionだけを読むテストを受け入れ条件にしない。

## 8. レビューの機械的チェック

PRテンプレートまたはレビューで次を確認する。

- queue項目に owner ID があるか。destroyer に対応する cancel があるか。
- deferred consumer が実行時に対象を再検証するか。
- user-facing string/path を map key や遅延identityとして使っていないか。
- session read/write/resolve の discard が `SessionReport` を通るか。
- session値を共通parser以外で読んでいないか。
- UI label/stale判定が effective state ではなく mode flag を見ていないか。
- 新しい非同期テストに interleaving、cancel、hostile input が含まれるか。

このチェックで見つかった迂回路は、コメントで正当化して残すのではなく、authority/APIへ寄せる。
