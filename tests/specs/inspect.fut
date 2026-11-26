// The state inspector's feed (std/reactive's watch): each named state as it is made, and as a view's go.
import "std/ui"
import "std/reactive" as r

state count = 1
state items = ["a", "b"]

view Row(label) {
  state open = false
  button id: "r-{label}" "{label}: {open}" on click { open = !open }
}
view Page {
  column {
    for i in items key i { Row(i) }
  }
}
mount(Page)

test "watch reports each named state, and each view's as it comes and goes" {
  var events = []  // (one list: the closure keeps this one)
  var seen = {}
  r.watch(fn(name, s, gone) {
    if gone { events.push("-{name}") } else { events.push("+{name}")  seen[name] = s }
  })
  expect events == ["+count", "+items", "+Row.open", "+Row.open#2"]
  expect seen["count"].get() == 1
  ui.trigger(ui.find("#r-b"), "click", nil)
  expect seen["Row.open#2"].get() == true
  while events.len() > 0 { events.pop() }
  items = ["a", "b", "c"]
  ui.refresh()  // (a reload: the page is made again, its states named again from the first)
  var all = events.join(" ")
  expect all.has("+Row.open#3") && all.has("-Row.open#2")
  expect seen["Row.open#2"].get() == true  // (kept)
}
