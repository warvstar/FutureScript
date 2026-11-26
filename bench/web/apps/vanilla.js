// The keyed table written straight against the DOM: the floor the others are measured from.
import { build, buttons } from "./data.js";

const main = document.getElementById("main");
const bar = document.createElement("div");
bar.className = "jumbotron";
for (const [id, label] of buttons) {
  const b = document.createElement("button");
  b.id = id, (b.textContent = label), bar.append(b);
}
const table = document.createElement("table"), tbody = document.createElement("tbody");
table.className = "table", (tbody.id = "tbody"), table.append(tbody), main.append(bar, table);
const proto = document.createElement("tr");
proto.innerHTML = '<td class="col-md-1"></td><td class="col-md-4"><a class="lbl"></a></td><td class="col-md-1"><a class="remove">x</a></td>';
let rows = [], selected = null;  // rows: {id, label, tr}
function tr(d) {
  const t = proto.cloneNode(true);
  t.firstChild.textContent = d.id, (t.childNodes[1].firstChild.textContent = d.label), (t.id = d.id);
  return { ...d, tr: t };
}
function append(ds) {
  const f = document.createDocumentFragment(), made = ds.map(tr);
  for (const r of made) f.append(r.tr);
  tbody.append(f), (rows = rows.concat(made));
}
function clear() { tbody.textContent = "", (rows = []), (selected = null); }
const ops = {
  run: () => (clear(), append(build(1000))),
  runlots: () => (clear(), append(build(10000))),
  add: () => append(build(1000)),
  update: () => { for (let i = 0; i < rows.length; i += 10) rows[i].tr.childNodes[1].firstChild.textContent = rows[i].label += " !!!"; },
  clear,
  swaprows: () => {
    if (rows.length < 999) return;
    const a = rows[1], b = rows[998], afterB = b.tr.nextSibling;
    tbody.insertBefore(b.tr, a.tr), tbody.insertBefore(a.tr, afterB);
    (rows[1] = b), (rows[998] = a);
  },
};
bar.addEventListener("click", (e) => ops[e.target.id]?.());
tbody.addEventListener("click", (e) => {
  const t = e.target.closest("tr"), i = rows.findIndex((r) => r.tr === t);
  if (e.target.classList.contains("lbl")) {
    selected?.classList.remove("danger"), t.classList.add("danger"), (selected = t);
  } else if (e.target.classList.contains("remove")) {
    if (selected === t) selected = null;
    t.remove(), rows.splice(i, 1);
  }
});
