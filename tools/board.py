"""docs/tasks.csv -> a numbered list, as a page and as markdown.

    python tools/board.py

Writes build-board/board.html (open it in a browser) and build-board/board.md.

Both come out of this one script so the numbers always agree: if the page says
12 and the chat says 12, they are the same row. That is the whole point -- the
number is how an instruction gets pointed at a row.

The numbering is positional, so it shifts when rows are added or removed. It
identifies a row within one rendering, not for ever; name the item too if the
instruction has to survive the day.
"""
import csv, io, json, pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "docs" / "tasks.csv"
BASE = ROOT / "build-board"
BASE.mkdir(exist_ok=True)

rows = list(csv.reader(io.StringIO(SRC.read_bytes().decode("utf-8-sig"))))
head, body = rows[0], [r for r in rows[1:] if r]
assert head == ["分類", "項目", "内容", "参照", "実装ブランチ", "備考"], head

ORDER = ["残課題【Fable】", "レビュー必要項目", "残課題", "暫定対応したが後で直す"]
open_rows = [r for r in body if r[0] != "対応済み"]
open_rows.sort(key=lambda r: ORDER.index(r[0]) if r[0] in ORDER else 99)

data = []
for i, r in enumerate(open_rows, 1):
    data.append({"n": i, "cat": r[0], "item": r[1], "body": r[2],
                 "ref": r[3], "branch": r[4], "note": r[5]})

# --- the page ---------------------------------------------------------------
HTML = """<title>viewer 課題一覧</title>
<style>
  :root { color-scheme: light dark; }
  body { margin:0; padding:16px;
         font-family:"Hiragino Kaku Gothic ProN","Yu Gothic",Meiryo,system-ui,sans-serif;
         font-size:13.5px; line-height:1.5; }
  h1 { font-size:14px; margin:0 0 2px; font-weight:600; }
  .sub { opacity:.65; font-size:12px; font-variant-numeric:tabular-nums; margin-bottom:10px; }
  input { width:100%; box-sizing:border-box; padding:6px 9px; font:inherit; font-size:12.5px;
          margin-bottom:10px; }
  table { border-collapse:collapse; width:100%; }
  th { text-align:left; font-size:11px; opacity:.6; font-weight:600; padding:5px 8px;
       border-bottom:1px solid currentColor; white-space:nowrap; }
  td { padding:6px 8px; vertical-align:top; border-bottom:1px solid rgba(128,128,128,.25); }
  td.n { text-align:right; opacity:.5; font-variant-numeric:tabular-nums; width:2.5em; }
  td.cat { white-space:nowrap; opacity:.75; font-size:11.5px; width:9em; }
  .mono { font-family:ui-monospace,Consolas,monospace; font-size:11.5px; opacity:.75; }
  .b { display:block; opacity:.6; font-size:12px; margin-top:2px;
       display:-webkit-box; -webkit-line-clamp:1; -webkit-box-orient:vertical; overflow:hidden; }
  tr.o .b { -webkit-line-clamp:unset; }
  .rdy { font-size:10.5px; border:1px solid currentColor; border-radius:3px;
         padding:0 4px; margin-left:5px; opacity:.8; white-space:nowrap; }
  footer { margin-top:14px; opacity:.55; font-size:11.5px; }
  code { background:rgba(128,128,128,.18); padding:1px 4px; border-radius:3px; }
</style>
<h1>viewer 課題一覧</h1>
<div class="sub" id="c"></div>
<input id="q" placeholder="検索">
<table><thead><tr><th></th><th>分類</th><th>項目</th><th>参照</th><th>ブランチ</th><th>備考</th></tr></thead>
<tbody id="t"></tbody></table>
<footer>番号で指示してください（「12番やって」）。正典は <code>docs/tasks.csv</code>、書き込みはそちらへ。</footer>
<script>
const D = __DATA__, $ = i => document.getElementById(i);
function esc(s){return (s||"").replace(/[&<>"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));}
function draw(){
  const q = $("q").value.trim().toLowerCase();
  const L = q ? D.filter(r => (r.n+" "+r.item+" "+r.body+" "+r.ref+" "+r.branch+" "+r.note)
                                .toLowerCase().includes(q)) : D;
  $("c").textContent = "未完了 " + D.length + " 件・うち " +
                       D.filter(r=>r.branch).length + " 件はブランチに実装済み" +
                       (q ? "　（表示 " + L.length + "）" : "");
  $("t").innerHTML = L.map(r =>
    '<tr onclick="this.classList.toggle(\\'o\\')">' +
    '<td class="n">'+r.n+'</td><td class="cat">'+esc(r.cat)+'</td>' +
    '<td>'+esc(r.item)+(r.branch?'<span class="rdy">実装済</span>':'') +
      (r.body?'<span class="b">'+esc(r.body)+'</span>':'')+'</td>' +
    '<td class="mono">'+esc(r.ref||"—")+'</td>' +
    '<td class="mono">'+esc(r.branch||"—")+'</td>' +
    '<td>'+esc(r.note||"—")+'</td></tr>').join("");
}
$("q").oninput = draw; draw();
</script>
"""
(BASE / "board.html").write_text(HTML.replace("__DATA__", json.dumps(data, ensure_ascii=False)),
                                 encoding="utf-8")

# --- the same list as markdown, same numbers --------------------------------
md = ["未完了 %d 件（対応済み %d 件）／ うち %d 件はブランチに実装済み"
      % (len(data), len(body) - len(data), sum(1 for d in data if d["branch"]))]
cur = None
for d in data:
    if d["cat"] != cur:
        cur = d["cat"]
        md += ["", "## %s" % cur, "", "| # | 項目 | 参照 | ブランチ | 備考 |", "|---|---|---|---|---|"]
    note = d["note"].replace("|", "/")
    if len(note) > 64:
        note = note[:64] + "…"
    md.append("| %d | %s | %s | %s | %s |"
              % (d["n"], d["item"].replace("|", "/"), d["ref"].replace("|", "/") or "—",
                 d["branch"].replace("|", "/") or "—", note or "—"))
(BASE / "board.md").write_text("\n".join(md), encoding="utf-8")
print("%d open rows -> board.html (%.0f kB) + board.md"
      % (len(data), (BASE / "board.html").stat().st_size / 1024))
