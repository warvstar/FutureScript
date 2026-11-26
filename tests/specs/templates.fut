// Views' elements as templates (src/views.cpp, ui.tpl): the fixed part cloned, the live parts wired (wasm only).
import "std/ui"

state n = 1
state on = false
state items = ["a", "b"]
state who = "Ada"

view Card {
  column .card .lit: on id: "card" {
    text .title "Count"
    text .value "{n}"
    p "{n > 2 ? "many" : "few"} of {7}"
    if on { b id: "flag" "on" } else { i "off" }
    ul { for x in items key x { li "{x}" } }
    check "Lit" bind: on id: "lit"
    button id: "more" "More" on click { n += 1 }
    span id: "who" "{who.len() > 3 ? who : who.slice(0, 1)}"
  }
}
mount(Card)

fn page() { return ui.htmlOf(ui.find("#card")) }

test "a view's element is cloned, and its live parts follow state" {
  expect page().has("<div id=\"card\" class=\"column card\">")  // the fixed attribute, then the live class
  expect page().has("<div class=\"text title\">Count</div><div class=\"text value\">1</div>")  // text needs no markers
  expect page().has("<p>few of 7</p>") && page().has("<i>off</i>")
  expect page().has("<li>a</li><li>b</li>")
  ui.trigger(ui.find("#more"), "click", nil)
  ui.trigger(ui.find("#more"), "click", nil)
  expect page().has("<div class=\"text value\">3</div>") && page().has("<p>many of 7</p>")
  ui.trigger(ui.find("#lit"), "change", {checked: true})  // a check (fut's own tag) inside a template
  expect page().has("<div id=\"card\" class=\"column card lit\">") && page().has("<b id=\"flag\">on</b>") && !page().has("<i>off</i>")
  items = ["b", "c", "a"]
  expect page().has("<li>b</li><li>c</li><li>a</li>")
}
test "a string with holes is one text node, set in place" {
  expect page().has("<span id=\"who\">A</span>")
  who = "Grace"
  expect page().has("<span id=\"who\">Grace</span>")
}
