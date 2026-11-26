// A page in fut: the hello-ui task list from AGE, on std/ui. Run it in the browser:
//   node web/serve.mjs   then open http://localhost:8000/?file=examples/hello-ui.fut
// The page is functions of signals: each part that shows one is redrawn alone when it changes.
import "std/ui"
import "std/reactive" as r

class Task {
  var serial
  var effort
  var done
  fn init(i) {
    self.serial = i + 1
    self.effort = r.signal(i % 5 + 1)
    self.done = r.signal(i % 3 == 0)
  }
}

fn more(effort) { if effort >= 5 { return 1 } return effort + 1 }       // 1 to 5 and around again
fn weight(effort) { if effort > 3 { return "heavy" } if effort > 1 { return "medium" } return "light" }

var tasks = r.signal([])
var name = r.signal("")
var clicks = r.signal(0)
var compact = r.signal(false)
var list = []
for i in 0..24 { list.push(Task(i)) }
tasks.set(list)

fn row(t) {
  var cls = fn() {
    var c = "task"
    if t.done.get() { c += " done" }
    if compact.get() { c += " compact" }
    return c
  }
  return ui.el("div", {"class": cls}, [
    ui.check(t.done, "Task {t.serial}", nil),
    ui.el("span", {"class": "effort"}, fn() { return "{weight(t.effort.get())} ({t.effort.get()})" }),
    ui.el("button", {id: "more{t.serial}", onclick: fn(e) { t.effort.update(more) }}, "More effort")
  ])
}

fn page() {
  return ui.el("div", {"class": "page"}, [
    ui.el("h1", {"class": "title"}, "Hello, fut"),
    ui.el("p", {"class": "muted"}, "This page is fut functions over signals, on std/ui: no virtual DOM, each part redrawn alone."),
    ui.el("label", nil, ["Your name ", ui.field(name, {id: "name"})]),
    ui.el("p", nil, fn() {
      if name.get().len() > 0 { return "Hi, {name.get()}! You clicked {clicks.get()} times." }
      return "Type your name, then click."
    }),
    ui.el("div", {"class": "bar"}, [
      ui.el("button", {id: "click", onclick: fn(e) { clicks.update(fn(c) { return c + 1 }) }}, "Click me"),
      ui.check(compact, "Compact rows", {id: "compact"})
    ]),
    ui.el("div", {"class": "list"}, ui.each(fn() { return tasks.get() }, fn(t) { return t.serial }, row))
  ])
}

ui.style("
.page \{ display: flex; flex-direction: column; gap: 12px; padding: 24px; max-width: 688px; }
.title \{ font-size: 28px; font-weight: bold; margin: 0; }
.muted \{ opacity: 0.7; margin: 0; }
.bar \{ display: flex; gap: 12px; align-items: center; }
.list \{ height: 320px; overflow: auto; }
.task \{ display: flex; gap: 12px; align-items: center; padding: 4px; }
.effort \{ flex-grow: 1; }
.done \{ opacity: 0.5; }
.compact \{ font-size: 12px; }
")
var target = ui.find("#ui")  // the page web/index.html serves; anywhere else, the whole body
if target == nil { target = "body" }
ui.mount(target, page)
