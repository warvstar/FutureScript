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
extern dom fn css(str): any
extern dom fn remove(any): void
extern dom fn base(str): void
extern dom fn html(any): str
extern dom fn fire(any, str, any): void
extern dom fn fake(str, any): void
extern dom fn request(any, fn): void
extern dom fn timer(num, any, fn): any
extern dom fn stopTimer(any): void
extern dom fn hash(): str
extern dom fn setHash(str): void
extern dom fn onHash(fn): void
extern dom fn stored(str): any
extern dom fn store(str, str): void
extern dom fn createSvg(str): any
extern dom fn modal(any, any): void
extern dom fn measure(any): any
extern dom fn template(any): num
extern dom fn instance(num, num): any

// What kind of child v is: "none" (nil, false, true), "text" (a string or number), "fn", "list" or "node".
fn kindOf(v) {
  var t = typeOf(v)
  if t == "nil" || t == "bool" { return "none" }
  if t == "function" { return "fn" }
  if t == "js value" { return "node" }
  if t == "list" { return "list" }
  return "text"
}

// Add child c to node p: text, a node, a list of children, or a function whose result is shown and kept current.
fn add(p, c) { addAt(p, nil, c) }
fn ins(p, before, n) {  // n into p, or before node before (when given)
  if before == nil { append(p, n) } else { put(before, n) }
}
fn addAt(p, before, c) {
  var k = kindOf(c)
  if k == "text" { ins(p, before, text(str(c))) }
  else if k == "node" { ins(p, before, c) }
  else if k == "list" { for x in c { addAt(p, before, x) } }
  else if k == "fn" { region(p, before, c, nil) }
}

// A part of p (or before node before) that shows f()'s result, and shows it again when what f read changes. Text
// is one text node (given: one already there), whose characters change; anything else goes between two markers,
// made when first needed, replacing what was there.
fn region(p, before, f, given) {
  var shown = {text: given, said: nil, start: nil, end: nil}
  if given != nil { shown.said = "" }
  r.effect(fn() {
    var v = f()
    var k = kindOf(v)
    if k == "text" && shown.text != nil {
      var t = str(v)
      if t != shown.said { setText(shown.text, t) shown.said = t }
      return
    }
    if k == "text" && shown.start == nil {  // the first time: no markers
      shown.said = str(v)
      shown.text = text(shown.said)
      ins(p, before, shown.text)
      return
    }
    if shown.start == nil {
      shown.start = anchor()
      shown.end = anchor()
      if shown.text == nil { ins(p, before, shown.start) ins(p, before, shown.end) }
      else { put(shown.text, shown.start) put(shown.text, shown.end) remove(shown.text) }  // the markers where the text was
    }
    clearBetween(shown.start, shown.end)
    shown.text = nil
    if k == "text" {
      shown.said = str(v)
      shown.text = text(shown.said)
      put(shown.end, shown.text)
    } else if k != "none" {
      var frag = fragment()
      add(frag, v)
      put(shown.end, frag)
    }
  })
}

// ---- templates (src/views.cpp): a view's element with its fixed part made once and cloned, the rest wired ----
var tplIds = {}  // key -> [template id, how many nodes an instance gives]
pub fn tpl(key: str, desc: fn, wire: fn): any {
  var t = nil
  if tplIds.has(key) { t = tplIds[key] }
  else {
    var d = desc()
    t = [template(d), 1 + wired(d) - d[2] % 2]  // (the root is wired as the first node)
    tplIds[key] = t
  }
  var n = instance(t[0], t[1])
  wire(n)
  return n[0]
}
fn wired(d) {  // how many of desc's nodes fut wires (see dom.js's template)
  if d == nil { return 1 }
  if typeOf(d) != "list" { return 0 }
  var c = d[2] % 2 + floor(d[2] / 2)
  for i in 3..d.len() { c += wired(d[i]) }
  return c
}
pub fn wire(n, props) { for k in props { prop(n, k, props[k]) } }  // a template's element: its live props
pub fn hole(marker, c) { addAt(nil, marker, c) }  // a template's place for a child
pub fn textAt(t, f: fn) { region(nil, nil, f, t) }  // a template's text, kept current

// Prop k of node n: "on<event>" takes a handler, given {type, value, checked, key}; a function shows its current
// result; "class" (a keyword: write {"class": ...}), "id" and other attributes take text (nil or false removes one); "value" and "checked" set the
// field's state.
fn prop(n, k, v) {
  if k.slice(0, 2) == "on" { listen(n, k.slice(2, k.len()), v) return }
  if kindOf(v) == "fn" {
    var last = {set: false, v: nil}
    r.effect(fn() {
      var x = v()
      if last.set && x == last.v { return }  // as it was: the DOM isn't touched
      setProp(n, k, x)
      last.set = true
      last.v = x
    })
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
// same: changing one item redraws that item, and reordering moves only the items out of place (those not on the
// longest run that kept its order).
pub fn each(items: fn, key: fn, view: fn): any {
  var frag = fragment()
  var start = anchor()
  var end = anchor()
  append(frag, start)
  append(frag, end)
  var st = {keys: [], shown: {}}  // keys in page order; key -> {item, start, end, dispose}
  r.onCleanup(fn() { for k in st.shown.keys() { st.shown[k].dispose() } })
  r.effect(fn() {
    var list = items()
    r.untrack(fn() { place(list, key, view, st, start, end) })
  })
  return [frag]  // (in a list: no pub function gives a bare fragment, so a "node" is one element, as place() takes it)
}
fn place(list, key, view, st, start, end) {
  var old = st.shown
  var keys = []
  var next = {}
  var kept = 0
  for item in list {
    var k = key(item)
    if next.has(k) { throw "each: two items have the key {k}" }
    var e = false  // false: a new item's (made below)
    if old.has(k) { e = old[k] }
    if e && e.item != item { e.dispose() removeRange(e.start, e.end) old.remove(k) e = false }
    if e { kept += 1 }
    next[k] = e
    keys.push(k)
  }
  if kept == 0 && old.len() > 0 {  // nothing stays: everything goes at once
    for k in old.keys() { old[k].dispose() }
    clearBetween(start, end)
  } else {
    for k in old.keys() { if !next.has(k) { old[k].dispose() removeRange(old[k].start, old[k].end) } }
  }
  // what stays put: the longest run of kept items in their old order; the rest move (or are new) into place
  var was = {}
  var i = 0
  for k in st.keys { if next.has(k) && next[k] { was[k] = i i += 1 } }
  var stay = steady(keys, was)
  var made = {}  // key -> the new item's nodes, made in order (its view may do things in order), placed below
  for j in 0..keys.len() {
    if next[keys[j]] { continue }
    var item = list[j]
    var e = {item: item, start: nil, end: nil, dispose: nil}
    e.dispose = r.root(fn() {
      var v = view(item)
      if kindOf(v) == "list" && v.len() == 1 { v = v[0] }  // (a view's block: a list)
      if kindOf(v) == "node" {  // one element: it is its own start and end
        e.start = v
        e.end = v
        made[keys[j]] = v
        return
      }
      var f = fragment()  // anything else goes between two markers
      e.start = anchor()
      e.end = anchor()
      append(f, e.start)
      add(f, v)
      append(f, e.end)
      made[keys[j]] = f
    })
    next[keys[j]] = e
  }
  var ref = end
  var j = keys.len() - 1
  while j >= 0 {
    var k = keys[j]
    var e = next[k]
    if made.has(k) { put(ref, made[k]) }
    else if !stay.has(k) { moveRange(e.start, e.end, ref) }
    ref = e.start
    j -= 1
  }
  st.keys = keys
  st.shown = next
}
// The keys (of those in was: key -> old position) on a longest increasing run of old positions, in new order.
fn steady(keys, was) {
  var tails = []  // tails[n]: index into keys of the smallest last position of a run of length n + 1
  var prev = []
  for i in 0..keys.len() {
    prev.push(-1)
    if !was.has(keys[i]) { continue }
    var p = was[keys[i]]
    var lo = 0
    var hi = tails.len()
    while lo < hi {
      var mid = floor((lo + hi) / 2)
      if was[keys[tails[mid]]] < p { lo = mid + 1 } else { hi = mid }
    }
    if lo > 0 { prev[i] = tails[lo - 1] }
    if lo == tails.len() { tails.push(i) } else { tails[lo] = i }
  }
  var out = {}
  if tails.len() == 0 { return out }
  var at = tails[tails.len() - 1]
  while at >= 0 { out[keys[at]] = true at = prev[at] }
  return out
}

// A text field showing s (a signal), which typing sets.
pub fn field(s: r.Signal, props): any { return bindText("input", s, props) }
fn bindText(tag, s, props) {
  var n = el(tag, props, nil)
  // What the box last said it holds isn't written back: its events may be handled after more typing (fut-mt.wasm
  // handles them on another thread), and writing an older value would undo that typing.
  var said = nil
  r.effect(fn() { if s.get() != said { setProp(n, "value", s.get()) } })
  listen(n, "input", fn(e) {
    said = e.value
    s.set(e.value)
  })
  listen(n, "blur", fn(e) { checked(s) })
  return n
}
fn checked(s) { if isa(s, FormField) { s.touched.set(true) } }  // a form's field, once left, shows what's wrong with it
fn problems(box, s) {  // under a form's field: what's wrong with it, once it shows
  if !isa(s, FormField) { return nil }
  r.effect(fn() { setProp(box, "aria-invalid", s.error() != nil) })
  return el("span", {"class": "field-error"}, fn() { return s.error() })
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

// A style sheet for the page. Sheets added before show() belong to that page: when show() runs again (as a reload
// runs the file's top level again), the sheets of the page before go, so an edited sheet replaces the old one.
pub fn style(sheet: str) {
  var n = css(sheet)
  sheets.push(n)
  return n
}
var sheets = []      // added since the last show()
var pageSheets = []  // the shown page's

// The HTML a node is now, and an event on it as a user would cause it ({value} typed, {checked} ticked): for tests.
pub fn htmlOf(n): str { return html(n) }
pub fn find(selector: str): any { return query(selector) }
pub fn trigger(n, event: str, data) { fire(n, event, data) }

// ---- data ----

// What load() is fetching, and what came back: data() (JSON parsed, else text), error() (nil, or what went wrong) and
// loading(), each followed as a signal is. The last data stays while a new request runs.
pub class Resource {
  fn init(source) {
    self.value = r.signal(nil)
    self.err = r.signal(nil)
    self.busy = r.signal(false)
    self.seq = 0
    self.source = source
    var me = self
    r.effect(fn() { me.send(source()) })
  }
  fn send(req) {
    if req == nil { return }
    self.seq += 1
    var mine = self.seq
    var me = self
    self.busy.set(true)
    request(req, fn(res) {
      if mine != me.seq { return }  // a newer request is on its way
      r.batch(fn() {
        me.busy.set(false)
        if res.status >= 200 && res.status < 300 { me.err.set(nil) me.value.set(decode(res.body)) }
        else if res.status == 0 { me.err.set(res.error) }
        else { me.err.set("the server answered {res.status}") }
      })
    })
  }
  fn data() { return self.value.get() }
  fn error() { return self.err.get() }
  fn loading() { return self.busy.get() }
  fn reload() { self.send(r.untrack(self.source)) }  // again, as it is now
}
fn decode(body) {
  var b = body.trim()
  if b.slice(0, 1) == "\{" || b.slice(0, 1) == "[" {
    try { return parseJson(b) } catch e { return body }
  }
  return body
}

// A Resource for source(): a URL, a request ({url, method, headers, body}), or nil (nothing yet). It fetches again when
// what source read changes. source may be a URL as it is.
pub fn load(source): Resource {
  if kindOf(source) == "fn" { return Resource(source) }
  return Resource(fn() { return source })
}

// What load() gets for url from now on (text, or {status, body}), instead of the network: for tests and demos.
pub fn respond(url: str, res) { fake(url, res) }

// ---- time ----

// f() every so many seconds, from now until the returned function is called or the view that made it goes away.
pub fn every(seconds: num, f: fn): fn { return ticker(seconds, true, f) }
// f() once, after so many seconds (unless stopped first).
pub fn after(seconds: num, f: fn): fn { return ticker(seconds, false, f) }
fn ticker(seconds, repeat, f) {
  var t = timer(seconds * 1000, repeat, fn() { r.batch(f) })
  var stop = fn() { stopTimer(t) }
  r.onCleanup(stop)
  return stop
}

// ---- the address: #/path ----

var route = nil
fn routeSignal() {
  if route == nil {
    route = r.signal(pathOf(hash()))
    onHash(fn() { route.set(pathOf(hash())) })
  }
  return route
}
fn pathOf(h) {
  if h == "" { return "/" }
  return h
}
// The page's path (the address after #, "/" if none), followed as a signal is.
pub fn path(): str { return routeSignal().get() }
// Go to path p (the address becomes #p).
pub fn go(p: str) { setHash(p) }
// The path's parts if it matches pattern, else nil: "/hosts/:id" matches "/hosts/web-1" as {id: "web-1"}; a last "*"
// matches the rest. Followed as a signal is, so a view's `if ui.at("/x") { }` follows the address.
pub fn at(pattern: str) {
  var want = parts(pattern)
  var got = parts(path())
  var found = {}
  var i = 0
  while i < want.len() {
    var w = want[i]
    if w == "*" { found["*"] = got.slice(i, got.len()).join("/") return found }
    if i >= got.len() { return nil }
    if w.slice(0, 1) == ":" { found[w.slice(1, w.len())] = got[i] }
    else if w != got[i] { return nil }
    i += 1
  }
  if i != got.len() { return nil }
  return found
}
fn parts(p) {
  var out = []
  for x in p.split("/") { if x != "" { out.push(x) } }
  return out
}

// ---- storage ----

// A signal that starts as what was saved under key (else v), and saves each new value: `state theme = ui.persist(...)`.
pub fn persist(key: str, v): r.Signal {
  var saved = stored(key)
  var s = r.signal(v)
  if saved != nil {
    try { s.set(parseJson(saved)) } catch e { }
  }
  r.root(fn() { r.effect(fn() { store(key, json(s.get())) }) })
  return s
}

// ---- SVG and charts ----

var SVG_TAGS = {svg: true, g: true, path: true, line: true, polyline: true, polygon: true, rect: true, circle: true,
  ellipse: true, defs: true, linearGradient: true, stop: true, title: true}

// An SVG element (tag, props, kids), as el() makes HTML ones. "label" makes SVG's <text>.
pub fn svg(tag: str, props, kids): any {
  var t = tag
  if t == "label" { t = "text" }
  var n = createSvg(t)
  if props != nil { for k in props { prop(n, k, props[k]) } }
  add(n, kids)
  return n
}

fn now(v) {
  if kindOf(v) == "fn" { return v() }
  return v
}
fn fmt(v) {  // short numbers for axes: 1200 -> 1.2k
  var a = v
  if a < 0 { a = -a }
  if a >= 1000000 { return "{floor(v / 100000) / 10}M" }
  if a >= 1000 { return "{floor(v / 100) / 10}k" }
  if a == floor(a) { return "{v}" }
  return "{floor(v * 10) / 10}"
}
fn niceMax(m) {  // a round number at or above m, for an axis's top
  if m <= 0 { return 1 }
  var step = 1
  while step * 10 <= m { step *= 10 }
  while step > m { step /= 10 }
  for k in [1, 2, 2.5, 5, 10] { if step * k >= m { return step * k } }
  return step * 10
}

// A small line of values, no axes: the trend beside a number. values: a list, or a function of one.
pub fn sparkline(values, props): any {
  return el("span", withClass(orEmpty(props), "spark"), fn() {
    var vs = now(values)
    if vs.len() < 2 { return nil }
    var lo = vs[0]
    var hi = vs[0]
    for v in vs { if v < lo { lo = v } if v > hi { hi = v } }
    if hi == lo { hi = lo + 1 }
    var pts = ""
    var i = 0
    for v in vs {
      pts += "{floor(i * 1000 / (vs.len() - 1)) / 10},{floor((28 - (v - lo) * 26 / (hi - lo)) * 10) / 10} "
      i += 1
    }
    return svg("svg", {viewBox: "0 0 100 30", preserveAspectRatio: "none"}, [svg("polyline", {points: pts}, nil)])
  })
}
fn orEmpty(props) {
  if props == nil { return {} }
  return props
}

// Lines over shared x labels: series is a list (or a function of one) of {label, values} (values: numbers, one per x
// label); opts: {labels: [...x labels], height} (labels may be a function too). A legend, a y axis from 0, and
// gridlines come with it; colors come from --c1..--c6.
pub fn lineChart(series, opts): any {
  return el("div", {"class": "chart"}, fn() { return plot(now(series), now(opts.labels), opts) })
}
// A bar per item, across, each with its label and value: items is a list (or a function of one) of {label, value}.
// opts: nil, or {unit} (shown after each value).
pub fn barChart(items, opts): any {
  return el("div", {"class": "chart bars"}, fn() {
    var list = now(items)
    var hi = 0
    for it in list { if it.value > hi { hi = it.value } }
    if hi == 0 { hi = 1 }
    var unit = ""
    if opts != nil && opts.has("unit") { unit = opts.unit }
    var rows = []
    for it in list {
      rows.push(el("div", {"class": "bar-row", title: "{it.label}: {it.value}{unit}"}, [
        el("span", {"class": "bar-name"}, it.label),
        el("span", {"class": "bar-track"}, el("i", {style: "width: {floor(it.value * 1000 / hi) / 10}%"}, nil)),
        el("span", {"class": "bar-value"}, "{fmt(it.value)}{unit}")]))
    }
    return rows
  })
}

fn plot(series, labels, opts) {
  var w = 600
  var h = 220
  if opts != nil && opts.has("height") { h = opts.height }
  var left = 40
  var bottom = 22
  var top = 8
  var ph = h - bottom - top
  var pw = w - left - 8
  var hi = 0
  var n = 0
  for s in series { for v in s.values { if v > hi { hi = v } } if s.values.len() > n { n = s.values.len() } }
  hi = niceMax(hi)
  var kids = []
  for k in [0, 1, 2, 3, 4] {  // gridlines and the y axis
    var y = top + ph - ph * k / 4
    kids.push(svg("line", {x1: left, x2: w - 8, y1: y, y2: y, "class": "grid"}, nil))
    kids.push(svg("label", {x: left - 6, y: y + 4, "class": "tick", "text-anchor": "end"}, fmt(hi * k / 4)))
  }
  var skip = 1
  while n / skip > 8 { skip += 1 }
  var slot = pw
  if n > 1 { slot = pw / (n - 1) }
  var i = 0
  for l in labels {  // x labels, thinned to fit
    if i % skip == 0 {
      var x = left + i * slot
      kids.push(svg("label", {x: x, y: h - 6, "class": "tick", "text-anchor": "middle"}, l))
    }
    i += 1
  }
  var c = 1
  for s in series {
    var pts = ""
    var j = 0
    for v in s.values {
      pts += "{left + j * slot},{top + ph - ph * v / hi} "
      j += 1
    }
    kids.push(svg("polyline", {points: pts, "class": "line", style: "stroke: var(--c{c})"}, nil))
    if s.values.len() > 0 {  // the latest value, marked
      var last = s.values[s.values.len() - 1]
      kids.push(svg("circle", {cx: left + (s.values.len() - 1) * slot, cy: top + ph - ph * last / hi, r: 3.5,
                               style: "fill: var(--c{c})"}, nil))
    }
    c = c % 6 + 1
  }
  var chart = svg("svg", {viewBox: "0 0 {w} {h}", "class": "plot"}, kids)
  if series.len() < 2 { return chart }
  var legend = []
  c = 1
  for s in series {
    legend.push(el("span", {"class": "key"}, [el("i", {style: "background: var(--c{c})"}, nil), s.label]))
    c = c % 6 + 1
  }
  return [el("div", {"class": "legend"}, legend), chart]
}

// ---- forms ----

// One of a Form's fields: get() and set() its value (so bind: takes it), and error(), what's wrong with it once it has
// been left or a submit was tried (else nil).
pub class FormField {
  fn init(form, name, v) {
    self.form = form
    self.name = name
    self.start = v
    self.value = r.signal(v)
    self.touched = r.signal(false)
  }
  fn get() { return self.value.get() }
  fn set(v) { self.value.set(v) }
  fn error() {
    if !self.touched.get() && !self.form.tried.get() { return nil }
    return self.form.check(self.name)
  }
}

// Fields with their first values, and rules: a rule takes a field's value and returns what's wrong with it, or nil.
//   var f = ui.form({email: ""}, {email: fn(v) { return v.has("@") ? nil : "Enter an email address." }})
//   form on submit { f.submit(fn(values) { save(values) }) } { field "Email" bind: f.at("email") button "Save" }
pub class Form {
  fn init(values, rules) {
    self.fields = {}
    self.rules = rules
    self.tried = r.signal(false)
    for k in values { self.fields[k] = FormField(self, k, values[k]) }
  }
  fn at(name: str): FormField {
    if !self.fields.has(name) { throw "this form has no field {name}" }
    return self.fields[name]
  }
  fn value(name: str) { return self.at(name).get() }
  fn values() {
    var m = {}
    for k in self.fields { m[k] = self.fields[k].get() }
    return m
  }
  fn check(name: str) {  // what's wrong with the field's value now, or nil
    if self.rules == nil || !self.rules.has(name) { return nil }
    var rule = self.rules[name]
    return rule(self.value(name))
  }
  fn error(name: str) { return self.at(name).error() }
  fn valid(): bool {
    for k in self.fields { if self.check(k) != nil { return false } }
    return true
  }
  // done(values) if every rule passes (true); else false, and every field shows what's wrong with it.
  fn submit(done: fn): bool {
    self.tried.set(true)
    if !self.valid() { return false }
    done(self.values())
    return true
  }
  fn reset() {  // the first values again, nothing shown as wrong
    var me = self
    r.batch(fn() {
      me.tried.set(false)
      for k in me.fields {
        var f = me.fields[k]
        f.value.set(f.start)
        f.touched.set(false)
      }
    })
  }
}
pub fn form(values, rules): Form { return Form(values, rules) }

// ---- toasts ----

var toastList = r.signal([])
var toastNext = 0
// A short message at the bottom of the page (show() puts them there), gone after a few seconds or when dismissed.
pub fn notify(text: str) {
  toastNext += 1
  var id = toastNext
  toastList.set(toastList.peek() + [{id: id, text: text}])
  timer(5000, false, fn() { dismiss(id) })
}
fn dismiss(id) {
  var keep = []
  for t in toastList.peek() { if t.id != id { keep.push(t) } }
  toastList.set(keep)
}
fn toasts() {
  return el("div", {"class": "toasts", "aria-live": "polite"}, each(fn() { return toastList.get() }, fn(t) { return t.id }, fn(t) {
    return el("div", {"class": "toast"}, [el("span", nil, t.text),
      el("button", {"class": "toast-x", "aria-label": "Dismiss", onclick: fn(e) { dismiss(t.id) }}, "Dismiss")])
  }))
}

// ---- long lists ----

// items (a list, or a function of one), each shown by view(item) in a row height pixels tall, in a box that scrolls
// (class "rows": give it a height; props: the box's, or nil). Only the rows in sight, and a few either side, are on the page, so a list of
// 100,000 costs what a screenful does. A row is drawn again when the item at its place changes.
pub fn rows(items, height: num, view: fn, props): any {
  var top = r.signal(0)
  var seen = r.signal(0)
  var box = el("div", withClass(orEmpty(props), "rows"), nil)
  var inner = el("div", {"class": "rows-inner", style: fn() { return "height: {now(items).len() * height}px" }}, nil)
  append(box, inner)
  listen(box, "scroll", fn(e) { r.batch(fn() { top.set(e.top) seen.set(e.height) }) })
  var drawn = {}  // index -> {item, node, dispose}
  r.onCleanup(fn() { for k in drawn.keys() { drawn[k].dispose() } })
  r.effect(fn() {
    var list = now(items)
    var h = seen.get()
    if h <= 0 { h = 800 }
    var first = floor(top.get() / height) - 5
    if first < 0 { first = 0 }
    var end = floor((top.get() + h) / height) + 5
    if end > list.len() { end = list.len() }
    r.untrack(fn() { drawRows(list, first, end, height, view, inner, drawn) })
  })
  after(0, fn() { seen.set(measure(box).height) })
  return box
}
fn drawRows(list, first, end, height, view, inner, drawn) {
  for k in drawn.keys() {
    var d = drawn[k]
    if k < first || k >= end || list[k] != d.item {
      d.dispose()
      remove(d.node)
      drawn.remove(k)
    }
  }
  var i = first
  while i < end {
    if !drawn.has(i) { drawn[i] = drawRow(list[i], i, height, view, inner) }
    i += 1
  }
}
fn drawRow(item, i, height, view, inner) {
  var slot = el("div", {"class": "row-slot", style: "top: {i * height}px; height: {height}px"}, nil)
  var dispose = r.root(fn() { add(slot, view(item)) })
  append(inner, slot)
  return {item: item, node: slot, dispose: dispose}
}

// ---- for views (src/views.cpp rewrites `view` and `state` into these) ----

// The list f fills: a view's block of children.
pub fn kids(f: fn): any {
  var k = []
  f(k)
  return k
}

// Class names from [name, on, name, on, ...]: the names whose on is true.
pub fn classes(parts): str {
  var out = ""
  var i = 0
  while i < parts.len() {
    if parts[i + 1] && parts[i] != "" {
      if out != "" { out += " " }
      out += parts[i]
    }
    i += 2
  }
  return out
}

fn withClass(props, base: str) {
  var c = props["class"]
  if c == nil { props["class"] = base }
  else if kindOf(c) == "fn" { props["class"] = fn() { return base + " " + c() } }
  else { props["class"] = base + " " + c }
  return props
}

// An element of a view: an HTML or SVG tag, or one of fut's own (row, column, text, field, check, link), with props
// (bind: a signal the element shows and sets) and children.
pub fn make(tag: str, props, kids): any {
  var bound = nil
  if props.has("bind") { bound = props.remove("bind") }
  if tag == "row" || tag == "column" || tag == "text" { return el("div", withClass(props, tag), kids) }
  if tag == "field" || tag == "choice" || tag == "area" {  // labelled: a text box, a select (its options: kids), a text area
    var box = nil
    var label = kids
    if tag == "choice" {  // choice "Label" bind: s { option ... }: the first child is the label
      var list = kids
      if kindOf(list) != "list" { list = [list] }
      label = list[0]
      box = el("select", nil, list.slice(1, list.len()))
      r.effect(fn() { setProp(box, "value", bound.get()) })
      listen(box, "change", fn(e) { bound.set(e.value) checked(bound) })
    } else {
      var t = "input"
      if tag == "area" { t = "textarea" }
      box = bindText(t, bound, nil)
    }
    for k in ["id", "placeholder", "type", "rows", "disabled"] { if props.has(k) { prop(box, k, props.remove(k)) } }
    return el("label", withClass(props, "field"), [el("span", nil, label), box, problems(box, bound)])
  }
  if tag == "check" {
    var box = el("input", nil, nil)
    setProp(box, "type", "checkbox")
    if props.has("id") { setProp(box, "id", props.remove("id")) }
    r.effect(fn() { setProp(box, "checked", bound.get()) })
    listen(box, "change", fn(e) { bound.set(e.checked) })
    return el("label", withClass(props, "check"), [box, el("span", nil, kids)])
  }
  if bound != nil && (tag == "input" || tag == "textarea") { return bindText(tag, bound, props) }
  if tag == "dialog" {  // bind: s, a signal of true or false: shown (modally) while true; Escape or the backdrop closes it
    var d = el("dialog", props, el("div", {"class": "dialog-body"}, kids))  // (a click on the dialog itself: the backdrop)
    if bound != nil {
      r.effect(fn() { modal(d, bound.get()) })
      listen(d, "close", fn(e) { bound.set(false) })
      listen(d, "click", fn(e) { if e.direct { bound.set(false) } })
    }
    return d
  }
  if tag == "link" {  // link to: "/path" "Text": an <a> to #/path, class "active" while the page is there
    var to = props.remove("to")
    var given = props["class"]
    props["href"] = fn() { return "#" + now(to) }
    props["class"] = fn() {
      var c = now(given)
      if c == nil { c = "" }
      if path() == now(to) { return (c + " active").trim() }
      return c
    }
    return el("a", props, kids)
  }
  if SVG_TAGS.has(tag) { return svg(tag, props, kids) }
  var n = el(tag, props, kids)
  if bound != nil && tag == "select" {
    r.effect(fn() { setProp(n, "value", bound.get()) })
    listen(n, "change", fn(e) { bound.set(e.value) })
  }
  return n
}

var themed = false
var shown = nil
var shownView = nil
// Show the page again, built by the code as it is now: after a reload (a file with views does this in its reload()
// unless it has its own). The state is kept; a view's own state starts again.
pub fn refresh() {
  if shownView != nil { show(shownView) }
}
// Show view() on the page (web/index.html's #ui, else the body), with fut's theme under the page's own styles. The
// page shown before (as a reload shows it again) is taken down first: its effects and timers stop.
pub fn show(view: fn): fn {
  if !themed { themed = true base(THEME) }
  if shown != nil { shown() }
  if sheets.len() > 0 {  // the file's top level ran again (else only functions changed: its sheets stay)
    for s in pageSheets { remove(s) }
    pageSheets = sheets
    sheets = []
  }
  var target = query("#ui")
  if target == nil { target = "body" }
  shownView = view
  shown = mount(target, fn() { return [view(), toasts()] })
  return shown
}

var THEME = "
:root \{ --bg: #fbfbfa; --fg: #1d1d1f; --muted: #6e6e73; --line: #e3e3e0; --accent: #3a5bd9; --accent-fg: #fff; --field: #fff;
  --radius: 8px; color-scheme: light dark; font: 15px/1.5 system-ui, -apple-system, 'Segoe UI', sans-serif; }
@media (prefers-color-scheme: dark) \{ :root \{ --bg: #161618; --fg: #ececee; --muted: #9a9aa0; --line: #2e2e32; --accent: #7d95f5;
  --accent-fg: #0d0d10; --field: #1f1f22; } }
body \{ margin: 0; background: var(--bg); color: var(--fg); }
.column \{ display: flex; flex-direction: column; gap: 12px; }
.row \{ display: flex; flex-wrap: wrap; align-items: center; gap: 12px; }
.text \{ margin: 0; }
.title \{ font-size: 28px; font-weight: 650; letter-spacing: -0.01em; }
.muted \{ color: var(--muted); }
button \{ font: inherit; padding: 6px 14px; border-radius: var(--radius); border: 1px solid var(--line); background: var(--field);
  color: var(--fg); cursor: pointer; transition: background .12s, border-color .12s; }
button:hover \{ border-color: var(--accent); }
button:active \{ background: var(--line); }
button.primary \{ background: var(--accent); color: var(--accent-fg); border-color: var(--accent); }
input:not([type]), input[type=text], input[type=search], textarea, select \{ font: inherit; color: inherit; background: var(--field);
  padding: 6px 10px; border: 1px solid var(--line); border-radius: var(--radius); outline: none; }
input:focus-visible, textarea:focus-visible, button:focus-visible \{ outline: 2px solid var(--accent); outline-offset: 1px; }
.field \{ display: flex; flex-direction: column; gap: 4px; font-size: 13px; color: var(--muted); }
.field input \{ font-size: 15px; color: var(--fg); }
.check \{ display: inline-flex; align-items: center; gap: 8px; cursor: pointer; }
input[type=checkbox] \{ accent-color: var(--accent); width: 16px; height: 16px; margin: 0; }
:root \{ --c1: #3a5bd9; --c2: #d9773a; --c3: #2f9e74; --c4: #a64fc4; --c5: #c9a227; --c6: #d94a6a; }
.chart \{ display: flex; flex-direction: column; gap: 6px; }
.plot \{ width: 100%; height: auto; overflow: visible; }
.plot .grid \{ stroke: var(--line); stroke-width: 1; }
.plot .tick \{ fill: var(--muted); font-size: 11px; font-variant-numeric: tabular-nums; }
.plot .line \{ fill: none; stroke-width: 2; stroke-linejoin: round; stroke-linecap: round; }
.bars \{ gap: 8px; }
.bar-row \{ display: grid; grid-template-columns: minmax(0, 9em) 1fr 4.5em; align-items: center; gap: 10px; font-size: 13px; }
.bar-name \{ overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.bar-track \{ height: 10px; border-radius: 5px; background: color-mix(in srgb, var(--line) 70%, transparent); overflow: hidden; }
.bar-track i \{ display: block; height: 100%; border-radius: 5px; background: var(--accent); }
.bar-value \{ color: var(--muted); font-variant-numeric: tabular-nums; text-align: right; }
.legend \{ display: flex; flex-wrap: wrap; gap: 4px 14px; font-size: 12px; color: var(--muted); }
.legend .key \{ display: inline-flex; align-items: center; gap: 6px; }
.legend i \{ width: 10px; height: 3px; border-radius: 2px; }
.spark \{ display: inline-block; width: 96px; height: 28px; }
.spark svg \{ width: 100%; height: 100%; display: block; overflow: visible; }
.spark polyline \{ fill: none; stroke: currentColor; stroke-width: 1.6; vector-effect: non-scaling-stroke; }
a.active \{ color: var(--accent); }
:root \{ --bad: #c8372d; }
@media (prefers-color-scheme: dark) \{ :root \{ --bad: #ff6b5b; } }
.field-error \{ color: var(--bad); font-size: 12.5px; min-height: 0; }
.field-error:empty \{ display: none; }
[aria-invalid=true] \{ border-color: var(--bad) !important; }
.field select, .field textarea \{ font-size: 15px; color: var(--fg); }
.dialog-body \{ padding: 22px 24px; }
dialog \{ border: 1px solid var(--line); border-radius: calc(var(--radius) + 4px); padding: 0; background: var(--bg);
  color: var(--fg); max-width: min(520px, calc(100vw - 32px)); width: 100%; box-shadow: 0 20px 60px rgb(0 0 0 / .25); }
dialog::backdrop \{ background: rgb(10 14 20 / .45); }
.toasts \{ position: fixed; bottom: 16px; left: 50%; transform: translateX(-50%); display: flex; flex-direction: column;
  gap: 8px; z-index: 10; width: max-content; max-width: calc(100vw - 32px); }
.toast \{ display: flex; align-items: center; gap: 14px; padding: 10px 10px 10px 16px; border-radius: var(--radius);
  background: var(--fg); color: var(--bg); box-shadow: 0 8px 24px rgb(0 0 0 / .2); }
.toast-x \{ background: transparent; color: inherit; border-color: transparent; opacity: .75; padding: 4px 8px; }
.rows \{ position: relative; overflow: auto; height: 480px; }
.rows-inner \{ position: relative; }
.row-slot \{ position: absolute; left: 0; right: 0; }
"
