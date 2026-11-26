import "./ui.fut"
import "std/reactive" as r

fn page(view) {  // in the document, so find() sees it
  var host = el("div", {id: "app"}, nil)
  mount(host, view)
  mount("body", fn() { return host })
  return host
}

test "static elements, props and text" {
  var n = el("p", {"class": "note", title: "a \"b\""}, ["Hello, ", el("b", nil, "fut"), 3])
  expect htmlOf(n) == "<p class=\"note\" title=\"a &quot;b&quot;\">Hello, <b>fut</b>3</p>"
}

test "a function child shows its current value; text changes in place" {
  var n = r.signal(0)
  var host = page(fn() { return el("span", nil, ["clicked ", fn() { return n.get() }, " times"]) })
  expect htmlOf(host) == "<div id=\"app\"><span>clicked 0 times</span></div>"
  n.set(5)
  expect htmlOf(host) == "<div id=\"app\"><span>clicked 5 times</span></div>"
}

test "events, fields, checkboxes" {
  var name = r.signal("")
  var clicks = r.signal(0)
  var done = r.signal(false)
  var btn = button("Click me", fn(e) { clicks.update(fn(c) { return c + 1 }) })
  var input = field(name, {placeholder: "Your name"})
  var box = check(done, "Done", {id: "cb"})
  var host = page(fn() {
    return el("div", nil, [input, btn, box, fn() {
      if name.get().len() > 0 { return "Hi, {name.get()}! You clicked {clicks.get()} times." }
      return "Type your name, then click."
    }, fn() { if done.get() { return el("i", nil, "done") } return nil }])
  })
  expect htmlOf(host).has("Type your name, then click.")
  trigger(input, "input", {value: "Ada"})
  trigger(btn, "click", nil)
  trigger(btn, "click", nil)
  expect name.get() == "Ada"
  expect htmlOf(host).has("Hi, Ada! You clicked 2 times.")
  expect htmlOf(input) == "<input placeholder=\"Your name\" value=\"Ada\">"
  name.set("Grace")
  expect htmlOf(input) == "<input placeholder=\"Your name\" value=\"Grace\">"
  trigger(find("#cb"), "change", {checked: true})
  expect done.get() && htmlOf(host).has("<i>done</i>")
}

test "reactive props" {
  var on = r.signal(false)
  var n = el("div", {"class": fn() { if on.get() { return "on" } return "off" }, hidden: fn() { return !on.get() }}, nil)
  expect htmlOf(n) == "<div class=\"off\" hidden></div>"
  on.set(true)
  expect htmlOf(n) == "<div class=\"on\"></div>"
}

test "each: any reordering, insertion and removal comes out in order" {
  var items = r.signal([])
  var made = {n: 0}
  var host = page(fn() {
    return el("p", nil, each(fn() { return items.get() }, fn(x) { return x }, fn(x) { made.n += 1 return el("i", nil, "{x}") }))
  })
  fn shows() {  // the numbers in the <i>s, in page order
    var out = []
    var parts = htmlOf(host).split("<i>")
    for i in 1..parts.len() { out.push(parts[i].split("</i>")[0]) }
    return out
  }
  for order in [[1, 2, 3, 4, 5, 6], [6, 5, 4, 3, 2, 1], [1, 2, 3, 4, 5, 6], [2, 1, 3, 4, 6, 5], [7, 1, 3, 8, 5, 9], [], [3, 1, 2], [1, 2, 3], [2, 3, 1]] {
    items.set(order)
    var want = []
    for x in order { want.push("{x}") }
    expect shows() == want
  }
  expect made.n == 12  // one view per key it hadn't shown just before: 1-6, then 7, 8, 9, then 3, 1, 2 after the list was empty
}
test "each: two items with one key is an error" {
  var items = r.signal([1, 1])
  expect throws { page(fn() { return el("p", nil, each(fn() { return items.get() }, fn(x) { return x }, fn(x) { return "{x}" })) }) }
}
test "each: keyed items, reused while unchanged" {
  var items = r.signal([{id: 1, t: "a"}, {id: 2, t: "b"}])
  var renders = []
  var host = page(fn() {
    return el("ul", nil, each(fn() { return items.get() }, fn(x) { return x.id }, fn(x) { renders.push(x.t) return el("li", nil, x.t) }))
  })
  expect htmlOf(host).has("<ul><!----><li>a</li><li>b</li><!----></ul>")  // one element an item: no markers of its own
  var l = items.get()
  items.set([l[1], l[0], {id: 3, t: "c"}])  // reorder and add: a and b are moved, not redrawn
  expect renders == ["a", "b", "c"]
  var h = htmlOf(host)
  expect h.find("<li>b</li>") < h.find("<li>a</li>") && h.find("<li>a</li>") < h.find("<li>c</li>")
  items.set([{id: 3, t: "c2"}])  // one changed (redrawn), two gone
  expect renders == ["a", "b", "c", "c2"]
  expect !htmlOf(host).has("<li>a</li>") && htmlOf(host).has("<li>c2</li>")
}

test "an item's own signals update just that item" {
  var a = r.signal(1)
  var b = r.signal(2)
  var items = r.signal([{id: "a", n: a}, {id: "b", n: b}])
  var renders = 0
  var host = page(fn() {
    return each(fn() { return items.get() }, fn(x) { return x.id }, fn(x) { renders += 0 return el("b", nil, fn() { return x.n.get() }) })
  })
  b.set(20)
  expect htmlOf(host).has("<b>1</b>") && htmlOf(host).has("<b>20</b>")
}
