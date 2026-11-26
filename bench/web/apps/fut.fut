// The keyed table of js-framework-benchmark, in fut views (bench/web/run.mjs drives it beside React and Solid).
import "std/ui"
import "std/lists"

var adjectives = ["pretty", "large", "big", "small", "tall", "short", "long", "handsome", "plain", "quaint", "clean",
  "elegant", "easy", "angry", "crazy", "helpful", "mushy", "odd", "unsightly", "adorable", "important", "inexpensive",
  "cheap", "expensive", "fancy"]
var colours = ["red", "yellow", "blue", "green", "pink", "brown", "purple", "brown", "white", "black", "orange"]
var nouns = ["table", "chair", "house", "bbq", "desk", "car", "pony", "cookie", "sandwich", "burger", "pizza", "mouse",
  "keyboard"]
var lcg = 1
fn pick(l) {  // the same labels each run, as the other apps
  lcg = (lcg * 16807) % 2147483647
  return l[lcg % l.len()]
}
class Item {
  state label
  fn init(id) { self.id = id  self.label = "{pick(adjectives)} {pick(colours)} {pick(nouns)}" }
}
var nextId = 1
fn build(n) {
  var l = []
  for i in 0..n { l.push(Item(nextId)) nextId += 1 }
  return l
}

state rows = []
state selected = 0

fn update() {
  var i = 0
  while i < rows.len() { var it = rows[i] it.label = it.label + " !!!" i += 10 }
}
fn swap() {
  if rows.len() < 999 { return }
  var l = rows.slice(0, rows.len())
  var a = l[1]
  l[1] = l[998]
  l[998] = a
  rows = l
}
fn drop(id) { rows = lists.filter(rows, fn(r) { return r.id != id }) }

view Line(r) {
  tr .danger: selected == r.id {
    td .col-md-1 "{r.id}"
    td .col-md-4 { a .lbl "{r.label}" on click { selected = r.id } }
    td .col-md-1 { a .remove "x" on click { drop(r.id) } }
  }
}

view Main {
  div .jumbotron {
    button id: "run" "Create 1,000 rows" on click { rows = build(1000) }
    button id: "runlots" "Create 10,000 rows" on click { rows = build(10000) }
    button id: "add" "Append 1,000 rows" on click { rows = rows + build(1000) }
    button id: "update" "Update every 10th row" on click { update() }
    button id: "clear" "Clear" on click { rows = [] }
    button id: "swaprows" "Swap rows" on click { swap() }
  }
  table .table { tbody id: "tbody" { for r in rows key r.id { Line(r) } } }
}

mount(Main)
