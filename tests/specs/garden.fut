// examples/garden.fut, driven as a user would (wasm only: std/ui is the DOM's).
import "../../examples/garden.fut"
import "std/ui"

fn page() { return ui.htmlOf(ui.find("body")) }
fn click(sel) { ui.trigger(ui.find(sel), "click", nil) }

test "Allotment in blocks: browse, filter, edit, move, note, remove" {
  expect page().has("Every bed") && page().has("18 plants, 4 ready to pick")
  expect page().has("Sungold") && page().has("Raised bed by the shed")
  click("#bed-b3")
  expect page().has("4 plants, none ready yet") && !page().has("Boltardy")
  ui.trigger(ui.find("#search"), "input", {value: "krim"})
  expect page().has("1 plant, none ready yet") && page().has("Black Krim") && !page().has("Sungold")
  ui.trigger(ui.find("#search"), "input", {value: "zzz"})
  expect page().has("Nothing here matches")
  click("#clear")
  expect page().has("4 plants") && !page().has("Nothing here matches")
  click("#plant-p1")
  expect page().has("id=\"drawer\"") && page().has("First truss setting")
  ui.trigger(ui.find("#name"), "input", {value: "Cherry tomato"})
  expect page().has("Cherry tomato")
  click("#to-harvest")
  expect page().has("Cherry tomato is now harvesting.") && page().has("4 plants, 1 ready to pick")
  ui.trigger(ui.find("#note"), "input", {value: "Picked the first bowl."})
  ui.trigger(ui.find("#note"), "keydown", {key: "Enter"})
  expect page().has("Picked the first bowl.") && page().has("11 May")
  ui.trigger(ui.find("#bed"), "change", {value: "b2"})
  expect page().has("3 plants")
  click("#close")
  expect !page().has("id=\"drawer\"")
  click("#show-board")
  click("#bed-b2")
  expect page().has("class=\"column lane\"") && page().has("Cherry tomato")
  click("#next-p6")
  expect page().has("Courgette is now planted out.")
  click("#stage-harvest")
  expect page().has("1 plant, 1 ready to pick")
  click("#add")
  expect page().has("Added a plant to South border.") && page().has("New plant")
  click("#remove")
  expect page().has("Removed New plant.") && !page().has("id=\"drawer\"")
}
