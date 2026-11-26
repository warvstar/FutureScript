// A view's own state across a reload (ui.refresh(), as a reload does): kept, by view and name, in the order made.
import "std/ui"

state items = ["a", "b", "c"]

view Counter(label) {
  state n = 0
  button id: "c-{label}" "{label}: {n}" on click { n += 1 }
}
view Page {
  column id: "page" {
    Counter("x")
    Counter("y")
    for i in items key i { Counter(i) }
    if items.len() > 3 { p "more" }
  }
}
mount(Page)

fn text(id) { return ui.htmlOf(ui.find("#{id}")) }
fn click(id) { ui.trigger(ui.find("#{id}"), "click", nil) }

test "a view's own state is kept when the page is shown again" {
  click("c-x")
  click("c-y")
  click("c-y")
  click("c-b")
  expect text("c-x").has("x: 1") && text("c-y").has("y: 2") && text("c-b").has("b: 1")
  ui.refresh()
  expect text("c-x").has("x: 1") && text("c-y").has("y: 2") && text("c-a").has("a: 0") && text("c-b").has("b: 1")
  items = ["a", "c"]  // b's row goes: its state with it
  ui.refresh()
  expect text("c-a").has("a: 0") && text("c-c").has("c: 0") && text("c-y").has("y: 2")
  click("c-c")
  ui.refresh()
  expect text("c-c").has("c: 1")
}
