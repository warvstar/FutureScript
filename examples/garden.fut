// Allotment: a season's plants, bed by bed, from seed to harvest. A whole app in fut's views.
//   node web/serve.mjs   then open http://localhost:8000/?file=examples/garden.fut
import "std/ui"
import "std/lists"
import "std/strings"

// ---- the garden ----
var STAGES = [
  {id: "seed", label: "Sown"},
  {id: "sprout", label: "Sprouted"},
  {id: "planted", label: "Planted out"},
  {id: "harvest", label: "Harvesting"}
]
var STAGE_FILTERS = [{id: "all", label: "All stages"}] + STAGES
var LIGHT = [{id: "full", label: "Full sun"}, {id: "part", label: "Part shade"}, {id: "shade", label: "Shade"}]
var MONTHS = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]
var MONTH_DAYS = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
var TODAY = 131  // 11 May, as days into the year

class Bed {
  fn init(id, title) { self.id = id self.title = title }
}
var beds = [Bed("b1", "Raised bed by the shed"), Bed("b2", "South border"), Bed("b3", "Greenhouse"), Bed("b4", "Herb spiral")]

class Note {
  fn init(id, day, text) { self.id = id self.day = day self.text = text }
}

class Plant {
  state name
  state variety
  state bed
  state stage
  state sun
  state notes
  fn init(id, name, variety, bed, stage, sun, sown) {
    self.id = id
    self.name = name
    self.variety = variety
    self.bed = bed
    self.stage = stage
    self.sun = sun
    self.sown = sown
    self.notes = []
  }
}

var nextId = 0
fn newId() {
  nextId += 1
  return "p{nextId}"
}
fn startingPlants() {
  var rows = [
    ["Tomato", "Sungold", "b3", "planted", "full", 62],
    ["Tomato", "Black Krim", "b3", "sprout", "full", 75],
    ["Basil", "Genovese", "b3", "seed", "full", 124],
    ["Chilli", "Hungarian Hot Wax", "b3", "sprout", "full", 58],
    ["Runner bean", "Scarlet Emperor", "b2", "seed", "full", 120],
    ["Courgette", "Defender", "b2", "sprout", "full", 110],
    ["Winter squash", "Crown Prince", "b2", "seed", "full", 126],
    ["Sweet pea", "Matucana", "b2", "planted", "full", 45],
    ["Lettuce", "Little Gem", "b1", "harvest", "part", 70],
    ["Beetroot", "Boltardy", "b1", "planted", "full", 96],
    ["Carrot", "Autumn King", "b1", "sprout", "full", 105],
    ["Broad bean", "Aquadulce", "b1", "planted", "full", 30],
    ["Garlic", "Germidour", "b1", "harvest", "full", 10],
    ["Chard", "Bright Lights", "b1", "sprout", "part", 112],
    ["Parsley", "Moss Curled", "b4", "planted", "part", 80],
    ["Thyme", "Common", "b4", "harvest", "full", 20],
    ["Chives", "Staro", "b4", "harvest", "part", 25],
    ["Mint", "Moroccan", "b4", "planted", "shade", 66]
  ]
  var list = []
  for r in rows { list.push(Plant(newId(), r[0], r[1], r[2], r[3], r[4], r[5])) }
  list[0].notes = [Note("n1", 118, "First truss setting. Pinched out the side shoots."), Note("n2", 128, "Fed with comfrey tea.")]
  list[8].notes = [Note("n3", 126, "Cutting the outer leaves; sow another row soon.")]
  list[12].notes = [Note("n4", 130, "Scapes are curling: snap them off for the bulbs.")]
  return list
}

state plants = startingPlants()
state selected = nil
state bedFilter = ""
state stageFilter = "all"
state lightFilter = "any"
state search = ""
state order = "name"
state mode = "list"
state dark = false
state draft = ""
state toast = ""

fn dateOf(day) {
  var m = 0
  var d = day
  while m < 11 && d > MONTH_DAYS[m] {
    d -= MONTH_DAYS[m]
    m += 1
  }
  return "{d} {MONTHS[m]}"
}
fn ago(day) {
  var n = TODAY - day
  if n <= 0 { return "today" }
  if n == 1 { return "yesterday" }
  if n < 14 { return "{n} days ago" }
  return "{int(n / 7)} weeks ago"
}
fn stageIndex(id) {
  for i in 0..STAGES.len() { if STAGES[i].id == id { return i } }
  return 0
}
fn stageLabel(id) { return STAGES[stageIndex(id)].label }
fn lightLabel(id) {
  var l = lists.find(LIGHT, fn(x) { return x.id == id })
  return l == nil ? "" : l.label
}
fn bedTitle(id) {
  var b = lists.find(beds, fn(x) { return x.id == id })
  return b == nil ? "No bed" : b.title
}

fn matches(p) {
  if bedFilter != "" && p.bed != bedFilter { return false }
  if stageFilter != "all" && p.stage != stageFilter { return false }
  if lightFilter != "any" && p.sun != lightFilter { return false }
  var q = strings.lower(search)
  return q == "" || strings.lower(p.name).has(q) || strings.lower(p.variety).has(q)
}
fn shown() {
  var l = lists.filter(plants, matches)
  if order == "sown" { return lists.sortBy(l, fn(p) { return -p.sown }) }
  if order == "stage" { return lists.sortBy(l, fn(p) { return -stageIndex(p.stage) }) }
  return lists.sortBy(l, fn(p) { return strings.lower(p.name + " " + p.variety) })
}
fn inStage(id) { return lists.filter(shown(), fn(p) { return p.stage == id }) }
fn inBed(id) { return lists.filter(plants, fn(p) { return p.bed == id }) }
fn progress(id) {  // how far the bed's plants have come, 0 to 100
  var l = inBed(id)
  if l.len() == 0 { return 0 }
  var total = 0
  for p in l { total += stageIndex(p.stage) }
  return int(total * 100 / (l.len() * 3))
}
fn varietyOf(p) { return p.variety == "" ? "No variety yet" : p.variety }
fn heading() { return bedFilter == "" ? "Every bed" : bedTitle(bedFilter) }
fn summary() {
  var l = shown()
  var ready = lists.count(l, fn(p) { return p.stage == "harvest" })
  var what = l.len() == 1 ? "plant" : "plants"
  return ready == 0 ? "{l.len()} {what}, none ready yet" : "{l.len()} {what}, {ready} ready to pick"
}

fn addPlant() {
  var p = Plant(newId(), "New plant", "", bedFilter == "" ? "b1" : bedFilter, "seed", "full", TODAY)
  plants = plants + [p]
  selected = p
  toast = "Added a plant to {bedTitle(p.bed)}. Name it on the right."
}
fn removePlant(p) {
  plants = lists.filter(plants, fn(x) { return x != p })
  selected = nil
  toast = "Removed {p.name}."
}
fn moveTo(p, i) {
  if i < 0 || i >= STAGES.len() { return }
  p.stage = STAGES[i].id
  toast = "{p.name} is now {strings.lower(STAGES[i].label)}."
}
fn addNote(p) {
  p.notes = p.notes + [Note(newId(), TODAY, draft)]
  draft = ""
}
fn clearFilters() {
  search = ""
  stageFilter = "all"
  lightFilter = "any"
}

// ---- the views ----
view App {
  row .app .dark: dark {
    Sidebar()
    column .main {
      Header()
      if mode == "list" { ListView() } else { Board() }
    }
    if selected != nil { Drawer(selected) }
    if toast != "" {
      row .toast {
        text "{toast}"
        button .link "Dismiss" on click { toast = "" }
      }
    }
  }
}

view Sidebar {
  column .side {
    row .brand {
      Growth("harvest")
      text .wordmark "Allotment"
    }
    text .season "Spring and summer 2027"
    button .primary .add id: "add" "Add a plant" on click { addPlant() }
    column .beds {
      button .bed .active: bedFilter == "" on click { bedFilter = "" } {
        text .bed-title "Every bed"
        text .bed-count "{plants.len()}"
      }
      for b in beds key b.id { BedLink(b) }
    }
    row .foot {
      check "Dark mode" bind: dark
    }
  }
}

view BedLink(b) {
  button .bed .active: bedFilter == b.id id: "bed-{b.id}" on click { bedFilter = b.id } {
    text .bed-title "{b.title}"
    text .bed-count "{inBed(b.id).len()}"
    div .meter {
      div .meter-fill style: "width: {progress(b.id)}%"
    }
  }
}

view Header {
  column .head {
    row .head-top {
      column .heading {
        h1 .page-title "{heading()}"
        text .summary "{summary()}"
      }
      input .search id: "search" type: "search" placeholder: "Search by name or variety" bind: search
      row .switch {
        button .switch-btn .active: mode == "list" id: "show-list" "List" on click { mode = "list" }
        button .switch-btn .active: mode == "board" id: "show-board" "Board" on click { mode = "board" }
      }
    }
    row .filters {
      for s in STAGE_FILTERS key s.id {
        button .chip .active: stageFilter == s.id id: "stage-{s.id}" "{s.label}" on click { stageFilter = s.id }
      }
      div .spacer
      label .pick {
        span "Light"
        select id: "light" bind: lightFilter {
          option "Any" value: "any"
          option "Full sun" value: "full"
          option "Part shade" value: "part"
          option "Shade" value: "shade"
        }
      }
      label .pick {
        span "Sort"
        select id: "order" bind: order {
          option "Name" value: "name"
          option "Newest sowing" value: "sown"
          option "Furthest along" value: "stage"
        }
      }
    }
  }
}

view ListView {
  column .list {
    if shown().len() == 0 {
      column .empty {
        text .empty-title "Nothing here matches"
        text .muted "Try another bed, or clear the search and filters."
        button id: "clear" "Clear filters" on click { clearFilters() }
      }
    }
    for s in STAGES key s.id { Group(s) }
  }
}

view Group(s) {
  if inStage(s.id).len() > 0 {
    column .group {
      row .group-head {
        div .stage-dot class: s.id
        h2 .group-title "{s.label}"
        text .group-count "{inStage(s.id).len()}"
      }
      for p in inStage(s.id) key p.id { Row(p) }
    }
  }
}

view Row(p) {
  div .plant .picked: selected == p id: "plant-{p.id}" tabindex: 0 on click { selected = p } {
    Growth(p.stage)
    column .who {
      text .name "{p.name}"
      text .variety "{varietyOf(p)}"
    }
    text .where "{bedTitle(p.bed)}"
    text .light "{lightLabel(p.sun)}"
    text .when "{ago(p.sown)}"
  }
}

view Growth(stage) {
  div .growth title: stageLabel(stage) {
    for s in STAGES key s.id {
      div .leaf class: s.id .grown: stageIndex(stage) >= stageIndex(s.id)
    }
  }
}

view Board {
  div .board {
    for s in STAGES key s.id {
      column .lane {
        row .lane-head {
          div .stage-dot class: s.id
          text .lane-title "{s.label}"
          text .group-count "{inStage(s.id).len()}"
        }
        for p in inStage(s.id) key p.id { Card(p) }
      }
    }
  }
}

view Card(p) {
  column .card .picked: selected == p id: "card-{p.id}" on click { selected = p } {
    text .name "{p.name}"
    text .variety "{p.variety}"
    row .card-foot {
      text .where "{bedTitle(p.bed)}"
      button .nudge disabled: stageIndex(p.stage) == 0 "Back" on click { moveTo(p, stageIndex(p.stage) - 1) }
      button .nudge id: "next-{p.id}" disabled: stageIndex(p.stage) == 3 "Next" on click { moveTo(p, stageIndex(p.stage) + 1) }
    }
  }
}

view Drawer(p) {
  column .drawer id: "drawer" {
    row .drawer-head {
      Growth(p.stage)
      button .link id: "close" "Close" on click { selected = nil }
    }
    field .big "Name" id: "name" bind: p.name
    field "Variety" id: "variety" bind: p.variety
    div .pair {
      label .field {
        span "Bed"
        select id: "bed" bind: p.bed {
          for b in beds key b.id {
            option "{b.title}" value: b.id
          }
        }
      }
      label .field {
        span "Light"
        select bind: p.sun {
          option "Full sun" value: "full"
          option "Part shade" value: "part"
          option "Shade" value: "shade"
        }
      }
    }
    column .stage-pick {
      span .label "Stage"
      div .stages {
        for s in STAGES key s.id {
          button .stage .current: p.stage == s.id id: "to-{s.id}" "{s.label}" on click { moveTo(p, stageIndex(s.id)) }
        }
      }
      text .muted "Sown {dateOf(p.sown)}, {ago(p.sown)}."
    }
    column .journal {
      h3 .journal-title "Notes"
      if p.notes.len() == 0 { text .muted "Nothing noted yet." }
      for n in lists.reverse(p.notes) key n.id {
        div .note {
          text .note-day "{dateOf(n.day)}"
          text .note-text "{n.text}"
        }
      }
      input .note-input id: "note" placeholder: "Write a note, then press Enter" bind: draft on keydown { if e.key == "Enter" && draft != "" { addNote(p) } }
    }
    button .danger id: "remove" "Remove plant" on click { removePlant(p) }
  }
}

ui.style("
@import url(https://fonts.googleapis.com/css2?family=Bricolage+Grotesque:opsz,wght@12..96,500..750&family=Instrument+Sans:wght@400..650&display=swap);
.app \{
  --paper: #eef2ec; --panel: #fbfcfa; --lane: #e3e9e1; --hover: #e4eae2; --ink: #1d2b22; --ink-2: #5b6a5f; --rule: #d3dccf;
  --moss: #2f6b45; --moss-ink: #f4f8f2; --moss-soft: #dbe9dc; --seed: #9a7754; --sprout: #86b450; --planted: #2f6b45;
  --harvest: #d6921c; --danger: #a8402c;
  --bg: var(--paper); --fg: var(--ink); --muted: var(--ink-2); --line: var(--rule); --field: var(--panel); --accent: var(--moss);
  --accent-fg: var(--moss-ink);
  display: flex; flex-wrap: wrap; align-items: stretch; gap: 0; height: 100%; min-height: 600px;
  container-type: inline-size; position: relative;
  background: var(--paper); color: var(--ink); border-radius: 14px; overflow: hidden;
  font: 14.5px/1.45 \"Instrument Sans\", system-ui, sans-serif; font-variant-numeric: tabular-nums;
}
.app.dark \{
  --paper: #121915; --panel: #19221c; --lane: #172019; --hover: #1d2820; --ink: #e2eadf; --ink-2: #8fa093; --rule: #27332a;
  --moss: #74c08b; --moss-ink: #0f1a13; --moss-soft: #1d3324; --seed: #b48f68; --sprout: #9cc865; --planted: #74c08b;
  --harvest: #e8a93a; --danger: #ef8a72;
}
.app h1, .app h2, .app h3 \{ margin: 0; font-family: \"Bricolage Grotesque\", system-ui, sans-serif; }
.app button \{ font-size: 14px; }

.side \{ width: 236px; flex: none; box-sizing: border-box; height: 100%; padding: 22px 14px 18px; gap: 16px; border-right: 1px solid var(--rule); }
.brand \{ gap: 10px; flex-wrap: nowrap; padding: 0 6px; }
.brand .growth \{ transform: scale(1.15); transform-origin: left center; }
.wordmark \{ font: 700 23px/1 \"Bricolage Grotesque\", system-ui, sans-serif; letter-spacing: -0.03em; font-variation-settings: \"opsz\" 96; }
.season \{ color: var(--ink-2); font-size: 13px; padding: 0 6px; margin-top: -8px; }
.add \{ width: 100%; padding: 9px 14px; }
.beds \{ gap: 2px; }
.bed \{
  display: grid; grid-template-columns: 1fr auto; gap: 7px 8px; align-items: center; text-align: left; width: 100%;
  padding: 9px 10px; border: 0; border-radius: 9px; background: transparent; color: var(--ink);
}
.bed:hover \{ background: var(--hover); }
.bed.active \{ background: var(--panel); box-shadow: 0 0 0 1px var(--rule); }
.bed-title \{ font-weight: 550; }
.bed-count \{ color: var(--ink-2); font-size: 13px; }
.meter \{ grid-column: 1 / -1; height: 4px; border-radius: 4px; background: var(--rule); overflow: hidden; }
.meter-fill \{ height: 100%; border-radius: 4px; background: var(--moss); transition: width .3s ease; }
.foot \{ margin-top: auto; padding: 0 6px; }

.main \{ flex: 1 1 0; min-width: 0; height: 100%; box-sizing: border-box; overflow: auto; padding: 30px 36px 40px; gap: 22px; }
.head \{ gap: 18px; }
.head-top \{ flex-wrap: nowrap; align-items: flex-end; gap: 14px; }
.heading \{ flex: 1; min-width: 0; gap: 4px; }
.page-title \{ font-size: 38px; line-height: 1.02; font-weight: 680; letter-spacing: -0.035em; font-variation-settings: \"opsz\" 96; }
.summary \{ color: var(--ink-2); }
.search \{
  width: 250px; font: inherit; color: var(--ink); background: var(--panel); border: 1px solid var(--rule); border-radius: 10px;
  padding: 8px 12px; outline: none;
}
.search:focus-visible, .note-input:focus-visible, .app select:focus-visible \{ outline: 2px solid var(--moss); outline-offset: 1px; }
.switch \{ flex-wrap: nowrap; gap: 2px; padding: 3px; border: 1px solid var(--rule); border-radius: 10px; background: var(--panel); }
.switch-btn \{ border: 0; background: transparent; padding: 5px 13px; border-radius: 7px; color: var(--ink-2); }
.switch-btn.active \{ background: var(--moss-soft); color: var(--moss); font-weight: 600; }
.filters \{ gap: 6px; }
.chip \{ border-radius: 999px; padding: 5px 13px; background: transparent; font-size: 13.5px; }
.chip.active \{ background: var(--ink); color: var(--paper); border-color: var(--ink); }
.spacer \{ flex: 1; }
.pick \{ display: inline-flex; align-items: center; gap: 7px; color: var(--ink-2); font-size: 13px; }
.app select \{
  font: inherit; font-size: 13.5px; color: var(--ink); background: var(--panel); border: 1px solid var(--rule); border-radius: 8px;
  padding: 5px 8px;
}

.list \{ gap: 28px; }
.group-wrap \{ display: contents; }
.group \{ gap: 0; }
.group-head \{ gap: 9px; flex-wrap: nowrap; padding: 0 8px 9px; border-bottom: 1px solid var(--rule); }
.group-title \{ font-size: 18px; font-weight: 620; letter-spacing: -0.015em; }
.group-count \{ color: var(--ink-2); font-size: 13px; }
.stage-dot \{ width: 9px; height: 9px; border-radius: 50%; background: var(--ink-2); }
.stage-dot.seed \{ background: var(--seed); }
.stage-dot.sprout \{ background: var(--sprout); }
.stage-dot.planted \{ background: var(--planted); }
.stage-dot.harvest \{ background: var(--harvest); }
.plant \{
  display: grid; grid-template-columns: 58px minmax(140px, 1fr) minmax(0, 150px) 84px 92px; gap: 16px; align-items: center;
  padding: 11px 8px; border-bottom: 1px solid var(--rule); cursor: pointer; outline: none;
}
.plant:hover \{ background: var(--hover); }
.plant:focus-visible \{ box-shadow: inset 0 0 0 2px var(--moss); }
.plant.picked \{ background: var(--moss-soft); box-shadow: inset 3px 0 0 var(--moss); }
.who \{ gap: 0; min-width: 0; }
.name \{ font-weight: 600; }
.variety, .where, .light, .when \{ color: var(--ink-2); font-size: 13px; }
.variety \{ white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }

.growth \{ display: flex; gap: 3px; align-items: center; }
.leaf \{ width: 11px; height: 11px; border-radius: 1px 11px; background: var(--rule); transition: background .25s ease; }
.leaf.grown.seed \{ background: var(--seed); }
.leaf.grown.sprout \{ background: var(--sprout); }
.leaf.grown.planted \{ background: var(--planted); }
.leaf.grown.harvest \{ background: var(--harvest); }

.board \{ display: flex; gap: 14px; align-items: flex-start; overflow-x: auto; padding-bottom: 6px; }
.lane \{ flex: 1; min-width: 196px; gap: 8px; padding: 12px; border-radius: 14px; background: var(--lane); }
.lane-head \{ gap: 8px; flex-wrap: nowrap; padding: 2px 4px 4px; }
.lane-title \{ font: 620 15px/1.2 \"Bricolage Grotesque\", system-ui, sans-serif; flex: 1; }
.card \{
  gap: 1px; padding: 11px 12px; border-radius: 10px; background: var(--panel); border: 1px solid var(--rule); cursor: pointer;
  transition: border-color .15s ease;
}
.card:hover \{ border-color: var(--ink-2); }
.card.picked \{ border-color: var(--moss); box-shadow: 0 0 0 1px var(--moss); }
.card-foot \{ margin-top: 8px; gap: 5px; flex-wrap: nowrap; }
.card-foot .where \{ flex: 1; min-width: 0; font-size: 12px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.nudge \{ padding: 2px 9px; font-size: 12px; border-radius: 6px; }
.nudge:disabled \{ opacity: .35; cursor: default; }

.drawer \{
  width: 368px; flex: none; overflow: auto; gap: 18px; padding: 22px 24px 24px; background: var(--panel);
  border-left: 1px solid var(--rule); animation: drawer-in .2s ease-out;
}
@keyframes drawer-in \{ from \{ transform: translateX(16px); opacity: 0; } }
.drawer-head \{ justify-content: space-between; flex-wrap: nowrap; }
.drawer-head .growth \{ gap: 5px; }
.drawer-head .leaf \{ width: 18px; height: 18px; border-radius: 2px 18px; }
.drawer .field \{ font-size: 12.5px; gap: 5px; }
.drawer .field input, .drawer .field select \{ width: 100%; box-sizing: border-box; font-size: 14.5px; padding: 7px 10px; }
.big input \{ font: 650 22px/1.2 \"Bricolage Grotesque\", system-ui, sans-serif !important; letter-spacing: -0.02em; padding: 6px 10px !important; }
.pair \{ display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }
.stage-pick \{ gap: 8px; }
.label \{ color: var(--ink-2); font-size: 12.5px; }
.stages \{ display: flex; flex-wrap: wrap; gap: 6px; }
.stage \{ padding: 5px 11px; border-radius: 999px; font-size: 13px; }
.stage.current \{ background: var(--moss); border-color: var(--moss); color: var(--moss-ink); font-weight: 600; }
.journal \{ gap: 10px; padding-top: 16px; border-top: 1px solid var(--rule); }
.journal-title \{ font-size: 16px; font-weight: 620; }
.note \{ display: grid; grid-template-columns: 52px 1fr; gap: 10px; align-items: baseline; }
.note-day \{ color: var(--ink-2); font-size: 12.5px; }
.journal .note-input \{
  font: inherit; color: var(--ink); background: var(--paper); border: 1px solid var(--rule); border-radius: 9px; padding: 8px 11px;
}
.danger \{ align-self: flex-start; margin-top: auto; color: var(--danger); background: transparent; border-color: transparent; padding-left: 0; }
.danger:hover \{ text-decoration: underline; border-color: transparent; }
.link \{ border: 0; background: transparent; color: var(--ink-2); padding: 4px 2px; }
.link:hover \{ color: var(--ink); text-decoration: underline; }

.empty \{ padding: 40px 8px; gap: 6px; align-items: flex-start; }
.empty-title \{ font: 620 21px/1.2 \"Bricolage Grotesque\", system-ui, sans-serif; }
.empty button \{ margin-top: 8px; }
.toast \{
  position: fixed; left: 50%; bottom: 26px; transform: translateX(-50%); flex-wrap: nowrap; gap: 16px; z-index: 5;
  padding: 10px 12px 10px 16px; border-radius: 10px; background: var(--ink); color: var(--paper);
  box-shadow: 0 10px 30px rgb(15 30 20 / 25%); animation: toast-in .18s ease-out;
}
.toast .link \{ color: var(--paper); opacity: .7; }
@keyframes toast-in \{ from \{ transform: translate(-50%, 8px); opacity: 0; } }
@media (prefers-reduced-motion: reduce) \{ .drawer, .toast \{ animation: none; } .meter-fill, .leaf \{ transition: none; } }

@container (max-width: 1060px) \{
  .drawer \{ position: absolute; top: 0; right: 0; bottom: 0; z-index: 4; box-shadow: -18px 0 44px rgb(10 25 15 / 22%); }
}
@container (max-width: 860px) \{
  .plant \{ grid-template-columns: 52px minmax(0, 1fr) minmax(0, 130px); }
  .light, .when \{ display: none; }
  .head-top \{ flex-wrap: wrap; }
  .search \{ order: 3; width: 100%; }
}
@container (max-width: 640px) \{
  .side \{ width: 100%; height: auto; border-right: 0; border-bottom: 1px solid var(--rule); }
  .beds \{ flex-direction: row; overflow-x: auto; }
  .bed \{ min-width: 168px; }
  .foot \{ margin-top: 0; }
  .main \{ height: auto; padding: 22px 16px; }
  .drawer \{ position: fixed; left: 0; width: auto; }
  .plant \{ grid-template-columns: 48px minmax(0, 1fr); }
  .where \{ display: none; }
}
")
mount(App)
