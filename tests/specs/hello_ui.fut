// examples/hello-ui.fut, driven as a user would (wasm: std/ui is the DOM's; under Node, dom.js's own document).
import "../../examples/hello-ui.fut"
import "std/ui"

fn page() { return ui.htmlOf(ui.find("body")) }

test "the AGE hello-ui task list, in fut" {
  expect page().has("Type your name, then click.")
  expect page().has("Task 24") && page().has("light (1)")
  ui.trigger(ui.find("#name"), "input", {value: "Ada"})
  ui.trigger(ui.find("#click"), "click", nil)
  expect page().has("Hi, Ada! You clicked 1 times.")
  expect !page().has("class=\"task compact\"")
  ui.trigger(ui.find("#compact"), "change", {checked: true})
  expect page().has("class=\"task done compact\"") && page().has("class=\"task compact\"")
  ui.trigger(ui.find("#more2"), "click", nil)
  ui.trigger(ui.find("#more2"), "click", nil)
  expect page().has("heavy (4)")
}
