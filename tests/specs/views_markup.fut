// examples/views-markup.fut, driven as a user would (wasm only: std/ui is the DOM's).
import "../../examples/views-markup.fut"
import "std/ui"

fn page() { return ui.htmlOf(ui.find("body")) }

test "the task list as views in markup" {
  expect page().has("Type your name, then click.")
  expect page().has("Task 24") && page().has("light (1)")
  ui.trigger(ui.find("#name"), "input", {value: "Ada"})
  ui.trigger(ui.find("#click"), "click", nil)
  expect page().has("Hi, Ada! You clicked 1 times.")
  ui.trigger(ui.find("#click"), "click", nil)
  expect page().has("Hi, Ada! You clicked 2 times.")
  expect !page().has("class=\"row task compact\"")
  ui.trigger(ui.find("#compact"), "change", {checked: true})
  expect page().has("class=\"row task done compact\"") && page().has("class=\"row task compact\"")
  ui.trigger(ui.find("#more2"), "click", nil)
  ui.trigger(ui.find("#more2"), "click", nil)
  expect page().has("heavy (4)")
  ui.trigger(ui.find("#name"), "input", {value: ""})
  expect page().has("Type your name, then click.")
}
