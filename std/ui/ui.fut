// Pages built from functions, kept up to date by std/reactive: a child or prop given as a function shows its
// current value, and only that part of the page changes when a signal the function read changes. No virtual DOM,
// no diffing of the whole view: each such function is an effect that updates its own nodes. The wasm build only
// (the DOM is JavaScript's); under Node, a small document of the module's own stands in, for tests.
//
//   var n = r.signal(0)
//   ui.mount("body", fn() {
//     return ui.el("div", nil, [ui.button("+1", fn(e) { n.update(fn(v) { return v + 1 }) }), fn() { return "clicked {n.get()}" }])
//   })
import "std/reactive" as r

extern dom fn create(str): any
extern dom fn text(str): any
extern dom fn anchor(): any
extern dom fn fragment(): any
extern dom fn setText(any, str): void
extern dom fn append(any, any): void
extern dom fn put(any, any): void
extern dom fn clear(any): void
extern dom fn clearBetween(any, any): void
extern dom fn moveRange(any, any, any): void
extern dom fn removeRange(any, any): void
extern dom fn setProp(any, str, any): void
extern dom fn listen(any, str, fn): void
extern dom fn query(str): any
extern dom fn css(str): void
extern dom fn html(any): str
extern dom fn fire(any, str, any): void

// What kind of child v is: "none" (nil, false, true), "text" (a string or number), "fn", "list" or "node".
fn kindOf(v) {
  if v == nil || v == false || v == true { return "none" }
  if v == str(v) { return "text" }
  var s = str(v)
  if s.slice(0, 4) == "<fn " { return "fn" }
  if s == "<js value>" { return "node" }
  if s.slice(0, 1) == "[" { return "list" }
  return "text"
}

// Add child c to node p: text, a node, a list of children, or a function whose result is shown and kept current.
fn add(p, c) {
  var k = kindOf(c)
  if k == "text" { append(p, text(str(c))) }
  else if k == "node" { append(p, c) }
  else if k == "list" { for x in c { add(p, x) } }
  else if k == "fn" { region(p, c) }
}

// A part of p that shows f()'s result between two markers, and shows it again when what f read changes. Text stays
// the same node (only its characters change); anything else replaces what was there.
fn region(p, f) {
  var start = anchor()
  var end = anchor()
  append(p, start)
  append(p, end)
  var shown = {text: nil}
  r.effect(fn() {
    var v = f()
    var k = kindOf(v)
    if k == "text" && shown.text != nil { setText(shown.text, str(v)) return }
    clearBetween(start, end)
    shown.text = nil
    if k == "text" {
      shown.text = text(str(v))
      put(end, shown.text)
    } else if k != "none" {
      var frag = fragment()
      add(frag, v)
      put(end, frag)
    }
  })
}

// Prop k of node n: "on<event>" takes a handler, given {type, value, checked, key}; a function shows its current
// result; "class" (a keyword: write {"class": ...}), "id" and other attributes take text (nil or false removes one); "value" and "checked" set the
// field's state.
fn prop(n, k, v) {
  if k.slice(0, 2) == "on" { listen(n, k.slice(2, k.len()), v) return }
  if kindOf(v) == "fn" {
    r.effect(fn() { setProp(n, k, v()) })
    return
  }
  setProp(n, k, v)
}

// An element: its tag, its props (a map, or nil), and its children (one child or a list).
pub fn el(tag: str, props, kids): any {
  var n = create(tag)
  if props != nil { for k in props { prop(n, k, props[k]) } }
  add(n, kids)
  return n
}

// Show view() in target (a selector or a node), replacing what was there. The returned function takes it down: its
// effects stop.
pub fn mount(target, view: fn): fn {
  var t = target
  if target == str(target) { t = query(target) }
  if t == nil { throw "mount: nothing matches {target}" }
  clear(t)
  return r.root(fn() { add(t, view()) })
}

// A list kept in step with items() (a list), one view per item, reused while the item's key(item) and value stay the
// same: changing one item redraws that item, and reordering moves nodes rather than redrawing them.
pub fn each(items: fn, key: fn, view: fn): fn {
  var shown = {}  // key -> {item, start, end, dispose}
  r.onCleanup(fn() { for k in shown.keys() { shown[k].dispose() } })
  return fn() {
    var list = items()
    var holder = fragment()
    var end = anchor()
    append(holder, end)
    r.untrack(fn() { place(list, key, view, shown, end) })
    return holder
  }
}
fn place(list, key, view, shown, end) {
  var next = {}
  for item in list {
    var k = key(item)
    var old = nil
    if shown.has(k) { old = shown.remove(k) }
    if old != nil && old.item == item {
      moveRange(old.start, old.end, end)
      next[k] = old
    } else {
      if old != nil { old.dispose() removeRange(old.start, old.end) }
      var frag = fragment()
      var start = anchor()
      var stop = anchor()
      append(frag, start)
      var dispose = r.root(fn() { add(frag, view(item)) })
      append(frag, stop)
      put(end, frag)
      next[k] = {item: item, start: start, end: stop, dispose: dispose}
    }
  }
  for k in shown.keys() { shown[k].dispose() removeRange(shown[k].start, shown[k].end) shown.remove(k) }
  for k in next { shown[k] = next[k] }
}

// A text field showing s (a signal), which typing sets.
pub fn field(s: r.Signal, props): any {
  var n = el("input", props, nil)
  r.effect(fn() { setProp(n, "value", s.get()) })
  listen(n, "input", fn(e) { s.set(e.value) })
  return n
}

// A checkbox showing s (a signal of true or false), which ticking sets, with its label (props: the box's).
pub fn check(s: r.Signal, label, props): any {
  var box = el("input", props, nil)
  setProp(box, "type", "checkbox")
  r.effect(fn() { setProp(box, "checked", s.get()) })
  listen(box, "change", fn(e) { s.set(e.checked) })
  return el("label", nil, [box, label])
}

// A button that calls onclick.
pub fn button(label, onclick: fn): any {
  return el("button", {onclick: onclick}, label)
}

// A style sheet for the page.
pub fn style(sheet: str) { css(sheet) }

// The HTML a node is now, and an event on it as a user would cause it ({value} typed, {checked} ticked): for tests.
pub fn htmlOf(n): str { return html(n) }
pub fn find(selector: str): any { return query(selector) }
pub fn trigger(n, event: str, data) { fire(n, event, data) }
