// std/ui's tables, tabs and menus, as views use them (wasm only).
import "std/ui"
import "std/lists"

state people = [{id: 1, name: "Ada", age: 36}, {id: 2, name: "Grace", age: 85}, {id: 3, name: "Alan", age: 41}]
state query = ""
state chosen = 0
state tab = "Overview"
var seen = {logs: 0, said: ""}

view Logs {
  p id: "logs" "Logs here"
}
fn logs() {
  seen.logs += 1
  return Logs()
}

view Page {
  column id: "page" {
    input id: "find" bind: query
    table id: "people" rows: people rowId: fn(p) { return "p{p.id}" } search: query empty: "Nobody." picked: fn(p) { return p.id == chosen } on pick { chosen = e.id } columns: [
      {key: "name", label: "Name"},
      {key: "age", label: "Age", num: true}]
    tabs bind: tab id: "tabs" {
      tab "Overview" { p id: "over" "Overview here" }
      tab "Logs" { logs() }
    }
    menu "Actions" id: "menu" {
      button id: "rename" "Rename" on click { seen.said = "rename" }
      button "Delete" on click { seen.said = "delete" }
    }
  }
}
mount(Page)

fn html(sel) { return ui.htmlOf(ui.find(sel)) }
fn names() {  // the table's names, in page order
  var h = html("#people")
  var at = []
  for n in ["Ada", "Grace", "Alan"] { if h.find("<td>{n}</td>") >= 0 { at.push({n: n, at: h.find("<td>{n}</td>")}) } }
  return lists.map(lists.sortBy(at, fn(x) { return x.at }), fn(x) { return x.n })
}

test "a table: its rows, sorted by a heading, searched, picked" {
  expect names() == ["Ada", "Grace", "Alan"]
  expect html("#people").has("<th scope=\"col\"><button class=\"sort\" id=\"people-name\">Name</button></th>")
  expect html("#people").has("<td class=\"num\">85</td>")
  ui.trigger(ui.find("#people-age"), "click", nil)
  expect names() == ["Ada", "Alan", "Grace"]
  expect html("#people").has("<th scope=\"col\" class=\"num\" aria-sort=\"ascending\">")
  ui.trigger(ui.find("#people-age"), "click", nil)
  expect names() == ["Grace", "Alan", "Ada"]
  expect html("#people").has("aria-sort=\"descending\"")
  ui.trigger(ui.find("#find"), "input", {value: "al"})
  expect names() == ["Alan"]
  ui.trigger(ui.find("#find"), "input", {value: "zz"})
  expect names().len() == 0 && html("#people").has("<tr class=\"table-empty\"><td colspan=\"2\">Nobody.</td></tr>")
  ui.trigger(ui.find("#find"), "input", {value: ""})
  expect html("#people").has("<tr class=\"table-empty\" hidden>")
  ui.trigger(ui.find("#p2"), "click", nil)
  expect chosen == 2 && ui.htmlOf(ui.find("#p2")).has(" aria-selected") && !ui.htmlOf(ui.find("#p1")).has(" aria-selected")
  ui.trigger(ui.find("#p3"), "keydown", {key: "Enter"})
  expect chosen == 3
  people = people + [{id: 4, name: "Barbara", age: 50}]
  expect html("#people").has("<td>Barbara</td>")
}

fn tabHtml(i) { return ui.htmlOf(ui.find("#tabs1-{i}")) }
test "tabs: the selected one's panel shows; a panel is made once; arrow keys move" {
  expect tabHtml(0).has("aria-selected=\"true\"") && tabHtml(0).has("tabindex=\"0\"")
  expect tabHtml(1).has("aria-selected=\"false\"") && tabHtml(1).has("tabindex=\"-1\"")
  expect seen.logs == 0  // made when first shown
  tab = "Logs"
  expect seen.logs == 1
  expect html("#tabs").has("aria-labelledby=\"tabs1-1\"><!----><p id=\"logs\">Logs here</p>")
  expect html("#tabs").has("id=\"tabs1-0-panel\" aria-labelledby=\"tabs1-0\" hidden>")
  ui.trigger(ui.find("#tabs1-1"), "keydown", {key: "ArrowRight"})
  expect tab == "Overview"
  ui.trigger(ui.find("#tabs1-0"), "keydown", {key: "End"})
  expect tab == "Logs"
  ui.trigger(ui.find("#tabs1-0"), "click", nil)
  expect tab == "Overview" && seen.logs == 1
}

test "a menu opens from its button and closes when an item is chosen" {
  expect html("#menu").has("<div class=\"menu-list\" role=\"menu\" hidden>")
  ui.trigger(ui.find("#menu-button"), "click", nil)
  expect html("#menu").has("aria-expanded=\"true\"") && html("#menu").has("role=\"menu\">")
  ui.trigger(ui.find("#rename"), "click", nil)
  expect seen.said == "rename" && html("#menu").has("role=\"menu\" hidden>")
}
