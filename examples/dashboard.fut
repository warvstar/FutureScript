// Switchyard: the on-call board for a small shop's services. Data from a URL, live numbers, pages by address,
// a sortable table and charts, all in fut's views.
//   node web/serve.mjs   then open http://localhost:8000/?file=examples/dashboard.fut
import "std/ui"
import "std/lists"
import "std/strings"
import "std/reactive" as r

var DATA = "/examples/data/fleet.json"
var fleet = ui.load(DATA)

state tick = 0          // how many live updates have come in: the charts' window moves on by one each
state hoursBack = 12    // hours shown
state live = true
state dark = ui.persist("switchyard.dark", false)
state reporting = false  // the report dialog is open
state reported = []      // incidents reported here, newest first

class Incident {
  state acked
  fn init(m) {
    self.id = m.id
    self.title = m.title
    self.service = m.service
    self.severity = m.severity
    self.opened = m.opened
    self.acked = m.acked
  }
}

// ---- the numbers ----

fn data() { return fleet.data() }
fn services() {
  if data() == nil { return [] }
  return data().services
}
fn service(id) { return lists.find(services(), fn(s) { return s.id == id }) }
var built = nil
fn incidents() {
  if data() == nil { return [] }
  if built == nil { built = lists.map(data().incidents, fn(m) { return Incident(m) }) }
  return reported + built
}
fn open() { return lists.filter(incidents(), fn(i) { return !i.acked }) }
fn openFor(id) { return lists.filter(incidents(), fn(i) { return i.service == id }) }

// A day's hourly values, turned on by tick hours and nudged a little, so the board moves as a live one would.
fn live24(values, salt) {
  var out = []
  for i in 0..24 {
    var v = values[(i + tick) % 24]
    var wobble = ((i * 7 + tick * 13 + salt * 5) % 11 - 5) / 100
    out.push(floor(v * (1 + wobble)))
  }
  return out
}
fn recent(list) { return list.slice(24 - hoursBack, 24) }
fn hours() {
  var hs = []
  for i in 0..24 { hs.push(data().hours[(i + tick) % 24]) }
  return recent(hs)
}
fn regionSeries() {
  var out = []
  var salt = 0
  for r in data().regions {
    out.push({label: r.name, values: recent(live24(r.rpm, salt))})
    salt += 1
  }
  return out
}
fn totals() {  // requests a minute, all regions, hour by hour
  var sums = []
  for v in regionSeries()[0].values { sums.push(0) }
  for s in regionSeries() {
    var i = 0
    for v in s.values { sums[i] += v i += 1 }
  }
  return sums
}
fn last(list) { return list[list.len() - 1] }
fn errorRate() {  // per cent of requests, weighted by traffic
  var reqs = 0
  var errs = 0
  for s in services() {
    reqs += s.rpm
    errs += s.rpm * s.errors / 100
  }
  if reqs == 0 { return 0 }
  return floor(errs * 1000 / reqs) / 10
}
fn medianP95() {
  var ps = lists.sort(lists.map(lists.filter(services(), fn(s) { return s.status != "down" }), fn(s) { return s.p95 }))
  if ps.len() == 0 { return 0 }
  return ps[floor(ps.len() / 2)]
}
fn troubled() { return lists.filter(services(), fn(s) { return s.status != "ok" }) }
fn headline() {
  var n = troubled().len()
  if n == 0 { return "Every service is running normally." }
  if n == 1 { return "1 of {services().len()} services needs attention." }
  return "{n} of {services().len()} services need attention."
}
fn thousands(n) {
  if n >= 1000 { return "{floor(n / 100) / 10}k" }
  return "{n}"
}
fn statusWord(s) {
  if s == "ok" { return "Normal" }
  if s == "degraded" { return "Degraded" }
  return "Down"
}
fn p95Bars() {
  var out = []
  for s in services() { if s.status != "down" { out.push({label: s.name, value: s.p95}) } }
  return lists.reverse(lists.sortBy(out, fn(b) { return b.value }))
}
fn latency(s) { return recent(live24(s.latency, s.name.len())) }

var COLUMNS = [{id: "name", label: "Service", num: false}, {id: "team", label: "Team", num: false},
               {id: "region", label: "Region", num: false}, {id: "status", label: "Status", num: false},
               {id: "rpm", label: "Req/min", num: true}, {id: "errors", label: "Errors %", num: true},
               {id: "p95", label: "p95 ms", num: true}]
fn ranked(q, only, by, desc) {
  var l = lists.filter(services(), fn(s) {
    if only != "all" && s.status != only { return false }
    var want = strings.lower(q)
    return q == "" || strings.lower(s.name).has(want) || strings.lower(s.team).has(want)
  })
  l = lists.sortBy(l, fn(s) { return s[by] })
  if desc { l = lists.reverse(l) }
  return l
}

// ---- reporting an incident ----

var report = ui.form({title: "", service: "", severity: "medium", details: ""}, {
  title: fn(v) {
    if v.trim().len() < 8 { return "Say what's wrong in a few words (8 characters or more)." }
    return nil
  },
  service: fn(v) {
    if v == "" { return "Pick the service it affects." }
    return nil
  }
})
var nextIncident = 419
fn fileReport(v) {
  var i = Incident({id: "INC-{nextIncident}", title: v.title.trim(), service: v.service, severity: v.severity,
                    opened: data().hours[(23 + tick) % 24], acked: false})
  nextIncident += 1
  reported = [i] + reported
  reporting = false
  report.reset()
  ui.notify("Reported {i.id}.")
}

// ---- the request log: made up the same way every time, 20,000 of them ----

var logCache = nil
fn requestLog() {
  if logCache != nil { return logCache }
  var paths = ["/api/cart", "/api/search?q=boots", "/api/login", "/img/p/4411.webp", "/api/orders/77812", "/api/recs/home"]
  var out = []
  var i = 0
  while i < 20000 {
    var s = services()[(i * 7 + floor(i / 13)) % services().len()]
    var roll = (i * 37 + 11) % 1000 / 10
    var code = 200
    if s.status == "down" { code = 503 }
    else if roll < s.errors { code = 500 }
    else if roll > 97.5 { code = 404 }
    var secs = 86399 - floor(i * 4.3)
    out.push({n: i, at: "{pad(floor(secs / 3600))}:{pad(floor(secs / 60) % 60)}:{pad(secs % 60)}", service: s.name, code: code,
              ms: floor(s.p95 * (0.3 + (i * 53 % 100) / 120)), path: paths[(i * 5) % paths.len()]})
    i += 1
  }
  logCache = out
  return out
}
fn pad(n) { return n < 10 ? "0{n}" : "{n}" }
fn commas(n) {  // 20000 -> "20,000"
  if n < 1000 { return "{n}" }
  return "{commas(floor(n / 1000))},{strings.padStart("{n % 1000}", 3, "0")}"
}
fn filteredLog(only, who) {
  return lists.filter(requestLog(), fn(e) { return (only == "all" || e.code >= 500) && (who == "" || e.service == who) })
}

// ---- the pages ----

view App {
  var stop = ui.every(3, fn() { if live { tick += 1 } })
  div .app .dark: dark {
    Rail()
    main .main {
      Top()
      if fleet.error() != nil { Problem() }
      else if data() == nil { p .quiet "Loading the fleet." }
      else if ui.at("/services/:id") != nil { ServicePage(ui.at("/services/:id").id) }
      else if ui.at("/services") != nil { Services() }
      else if ui.at("/incidents") != nil { Incidents() }
      else if ui.at("/requests") != nil { Requests() }
      else { Overview() }
    }
  }
}

view Rail {
  nav .rail {
    div .brand {
      span .mark
      "Switchyard"
    }
    link .nav to: "/" id: "nav-overview" "Overview"
    link .nav to: "/services" id: "nav-services" "Services"
    link .nav to: "/incidents" id: "nav-incidents" {
      "Incidents"
      if open().len() > 0 { span .count "{open().len()}" }
    }
    link .nav to: "/requests" id: "nav-requests" "Requests"
    div .rail-foot {
      check "Dark" bind: dark
    }
  }
}

view Top {
  header .top {
    div .ranges {
      for h in [6, 12, 24] key h {
        button .range .on: hoursBack == h id: "range-{h}" "{h}h" on click { hoursBack = h }
      }
    }
    check "Live" id: "live" bind: live
    button .ghost id: "refresh" disabled: fleet.loading() on click { fleet.reload() } {
      if fleet.loading() { "Refreshing" } else { "Refresh" }
    }
  }
}

view Problem {
  div .problem {
    h2 "The fleet data didn't load."
    p "{fleet.error()}. Check that the dev server is running from the repository root, then try again."
    button .primary "Try again" on click { fleet.reload() }
  }
}

view Overview {
  section .yard {
    h1 .headline id: "headline" "{headline()}"
    div .board {
      for s in services() key s.id {
        link .signal class: s.status to: "/services/{s.id}" id: "sig-{s.id}" {
          span .lamp
          span .sig-name "{s.name}"
          span .sig-meta "{s.region} · {statusWord(s.status)}"
        }
      }
    }
  }
  div .tiles {
    Tile("Requests a minute", fn() { return thousands(last(totals())) }, "across 4 regions", fn() { return totals() })
    Tile("Error rate", fn() { return "{errorRate()}%" }, "of all requests", nil)
    Tile("Median p95", fn() { return "{medianP95()} ms" }, "services that are up", nil)
    Tile("Open incidents", fn() { return open().len() }, "not yet acknowledged", nil)
  }
  div .charts {
    section .panel .wide {
      h2 "Requests a minute, by region"
      ui.lineChart(fn() { return regionSeries() }, {labels: fn() { return hours() }, height: 240})
    }
    section .panel {
      h2 "Slowest services, p95"
      ui.barChart(fn() { return p95Bars() }, {unit: " ms"})
    }
  }
}

// A number with its label; value (and trend, a list of numbers, if not nil) are functions, so the tile stays current.
view Tile(label, value, note, trend) {
  div .tile {
    span .tile-label "{label}"
    span .tile-value value()
    row .tile-foot {
      span .quiet "{note}"
      if trend != nil { ui.sparkline(trend, nil) }
    }
  }
}

view Services {
  state query = ""
  state only = "all"
  state sortKey = "name"
  state desc = false
  section .panel {
    row .table-tools {
      input .search id: "search" type: "search" placeholder: "Find a service or team" bind: query
      for f in ["all", "ok", "degraded", "down"] key f {
        button .chip .on: only == f id: "only-{f}" on click { only = f } {
          if f == "all" { "All" } else { "{statusWord(f)}" }
        }
      }
    }
    table .grid {
      thead {
        tr {
          for c in COLUMNS key c.id {
            th .num: c.num .sorted: sortKey == c.id id: "sort-{c.id}" on click { if sortKey == c.id { desc = !desc } else { sortKey = c.id desc = false } } {
              "{c.label}"
              if sortKey == c.id { span .arrow desc ? " ↓" : " ↑" }
            }
          }
        }
      }
      tbody {
        for s in ranked(query, only, sortKey, desc) key s.id {
          tr .pick id: "row-{s.id}" on click { ui.go("/services/{s.id}") } {
            td .name {
              span .lamp class: s.status
              "{s.name}"
            }
            td "{s.team}"
            td "{s.region}"
            td "{statusWord(s.status)}"
            td .num "{s.rpm}"
            td .num "{s.errors}"
            td .num "{s.p95}"
          }
        }
      }
    }
    if ranked(query, only, sortKey, desc).len() == 0 { p .quiet .empty "No service matches. Clear the search or pick All." }
  }
}

view ServicePage(id) {
  var s = service(id)
  if s == nil {
    section .panel {
      h2 "There is no service called {id}."
      link to: "/services" "See every service"
    }
  } else {
    link .back to: "/services" "All services"
    section .svc-head class: s.status {
      span .lamp class: s.status
      h1 "{s.name}"
      span .quiet "{s.team} · {s.region} · {statusWord(s.status)}"
    }
    div .tiles {
      Tile("Requests a minute", fn() { return thousands(s.rpm) }, "now", nil)
      Tile("Errors", fn() { return "{s.errors}%" }, "of its requests", nil)
      Tile("p95", fn() { return "{s.p95} ms" }, "the slowest 5% take longer", nil)
    }
    div .charts {
      section .panel .wide {
        h2 "p95 latency, ms"
        ui.lineChart(fn() { return [{label: s.name, values: latency(s)}] }, {labels: fn() { return hours() }, height: 200})
      }
      section .panel {
        h2 "Deploys today"
        ul .log {
          for d in s.deploys key d.at {
            li {
              span .at "{d.at}"
              span "{d.note}"
              span .quiet "{d.by}"
            }
          }
        }
        h2 "Incidents"
        if openFor(id).len() == 0 { p .quiet "None today." }
        for i in openFor(id) key i.id { IncidentRow(i) }
      }
    }
  }
}

view Incidents {
  state level = "all"
  section .panel {
    row .table-tools {
      for f in ["all", "high", "medium", "low"] key f {
        button .chip .on: level == f id: "level-{f}" on click { level = f } {
          if f == "all" { "All" } else { "{f}" }
        }
      }
      button .primary .push id: "report" "Report an incident" on click { reporting = true }
    }
    for i in lists.filter(incidents(), fn(x) { return level == "all" || x.severity == level }) key i.id { IncidentRow(i) }
  }
  Report()
}

view Report {
  dialog bind: reporting {
    form .report id: "report-form" on submit { report.submit(fileReport) } {
      h2 "Report an incident"
      field "What's wrong" id: "r-title" placeholder: "Checkout times out for EU cards" bind: report.at("title")
      row .pair {
        choice "Service" id: "r-service" bind: report.at("service") {
          option "Pick one" value: ""
          for s in services() key s.id { option "{s.name}" value: s.id }
        }
        choice "Severity" id: "r-severity" bind: report.at("severity") {
          option "High" value: "high"
          option "Medium" value: "medium"
          option "Low" value: "low"
        }
      }
      area "Details (optional)" id: "r-details" rows: 3 bind: report.at("details")
      row .actions {
        button .ghost type: "button" "Cancel" on click { reporting = false }
        button .primary id: "r-submit" "Report incident"
      }
    }
  }
}

view Requests {
  state only = "all"
  state who = ""
  state shown = r.computed(fn() { return filteredLog(only, who) })
  section .panel {
    row .table-tools {
      for f in ["all", "errors"] key f {
        button .chip .on: only == f id: "log-{f}" on click { only = f } {
          if f == "all" { "All" } else { "Errors" }
        }
      }
      select id: "log-service" bind: who {
        option "Every service" value: ""
        for s in services() key s.id { option "{s.name}" value: s.name }
      }
      span .quiet .push "{commas(shown.len())} requests today"
    }
    div .log-head {
      span "Time"
      span "Service"
      span "Status"
      span .num "ms"
      span "Path"
    }
    ui.rows(fn() { return shown }, 34, fn(e) { return LogRow(e) }, {id: "log-rows"})
  }
}

view LogRow(e) {
  div .log-row .bad: e.code >= 500 .miss: e.code == 404 {
    span .at "{e.at}"
    span "{e.service}"
    span .code "{e.code}"
    span .num "{e.ms}"
    span .path "{e.path}"
  }
}

view IncidentRow(i) {
  div .incident .acked: i.acked id: "inc-{i.id}" {
    span .sev class: i.severity "{i.severity}"
    div .inc-body {
      span .inc-title "{i.title}"
      span .quiet "{i.id} · opened {i.opened} · "
      link to: "/services/{i.service}" "{i.service}"
    }
    if i.acked { span .quiet "Acknowledged" }
    else { button .ghost id: "ack-{i.id}" "Acknowledge" on click { i.acked = true ui.notify("Acknowledged {i.id}.") } }
  }
}

ui.style("
@import url('https://fonts.googleapis.com/css2?family=Schibsted+Grotesk:wght@400;500;600;700;800&display=swap');
.app \{
  --bg: #e9edf0; --panel: #fbfcfc; --ink: #142028; --muted: #5a6872; --line: #d3dade; --accent: #1d4ed8;
  --ok: #17885a; --warn: #c07a0c; --down: #cf3b2c; --c1: #1d4ed8; --c2: #c07a0c; --c3: #17885a; --c4: #8a3ffc;
  --fg: var(--ink); --field: var(--panel); --accent-fg: #fff;
  display: grid; grid-template-columns: 200px 1fr; min-height: 100%; background: var(--bg); color: var(--ink);
  font: 14px/1.45 'Schibsted Grotesk', system-ui, sans-serif;
}
.app.dark \{
  --bg: #11171c; --panel: #182129; --ink: #e3e9ed; --muted: #8b99a3; --line: #27323b; --accent: #7aa2ff;
  --ok: #3fc489; --warn: #f0a93b; --down: #ff6b5b; --c1: #7aa2ff; --c2: #f0a93b; --c3: #3fc489; --c4: #b48cff;
  --accent-fg: #0b1220; color-scheme: dark;
}
.rail \{ display: flex; flex-direction: column; gap: 2px; padding: 20px 12px; border-right: 1px solid var(--line); }
.brand \{ display: flex; align-items: center; gap: 10px; font-weight: 800; font-size: 17px; letter-spacing: -0.02em; padding: 0 10px 18px; }
.mark \{ width: 14px; height: 14px; border-radius: 50%; background: var(--ok); box-shadow: 0 0 0 4px color-mix(in srgb, var(--ok) 22%, transparent); }
.nav \{ display: flex; justify-content: space-between; align-items: center; padding: 7px 10px; border-radius: 7px; color: var(--muted); text-decoration: none; font-weight: 500; }
.nav:hover \{ color: var(--ink); background: color-mix(in srgb, var(--ink) 6%, transparent); }
.nav.active \{ color: var(--ink); background: var(--panel); box-shadow: inset 3px 0 0 var(--accent); }
.count \{ background: var(--down); color: #fff; font-size: 11px; font-weight: 700; border-radius: 9px; padding: 0 7px; }
.rail-foot \{ margin-top: auto; padding: 10px; color: var(--muted); }
.main \{ padding: 18px 28px 40px; display: flex; flex-direction: column; gap: 20px; min-width: 0; }
.top \{ display: flex; flex-wrap: wrap; align-items: center; gap: 12px 16px; justify-content: flex-end; }
.ranges \{ display: inline-flex; border: 1px solid var(--line); border-radius: 8px; overflow: hidden; }
.range \{ border: 0; border-radius: 0; padding: 5px 12px; background: transparent; }
.range.on \{ background: var(--ink); color: var(--bg); }
button.ghost \{ background: transparent; }
.quiet \{ color: var(--muted); }
.yard \{ display: flex; flex-direction: column; gap: 14px; }
.headline \{ font-size: clamp(26px, 3.2vw, 40px); font-weight: 800; letter-spacing: -0.03em; line-height: 1.05; margin: 0; max-width: 22ch; }
.board \{ display: grid; grid-template-columns: repeat(auto-fill, minmax(150px, 1fr)); gap: 8px; }
.signal \{ display: grid; grid-template-columns: auto 1fr; column-gap: 10px; align-items: center; padding: 12px 14px;
  background: var(--panel); border: 1px solid var(--line); border-radius: 10px; color: inherit; text-decoration: none; transition: border-color .12s; }
.signal:hover \{ border-color: var(--ink); }
.signal .lamp \{ grid-row: span 2; width: 12px; height: 12px; }
.sig-name \{ font-weight: 600; }
.sig-meta \{ font-size: 12px; color: var(--muted); }
.signal.degraded \{ border-color: color-mix(in srgb, var(--warn) 55%, var(--line)); }
.signal.down \{ background: color-mix(in srgb, var(--down) 10%, var(--panel)); border-color: var(--down); }
.lamp \{ display: inline-block; width: 9px; height: 9px; border-radius: 50%; background: var(--ok); flex: none; }
.lamp.degraded, .signal.degraded .lamp \{ background: var(--warn); }
.lamp.down, .signal.down .lamp \{ background: var(--down); animation: blink 1.2s steps(2, start) infinite; }
@keyframes blink \{ to \{ opacity: .25; } }
@media (prefers-reduced-motion: reduce) \{ .lamp \{ animation: none !important; } }
.tiles \{ display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap: 12px; }
.tile \{ display: flex; flex-direction: column; gap: 2px; padding: 14px 16px; background: var(--panel); border: 1px solid var(--line); border-radius: 10px; }
.tile-label \{ color: var(--muted); font-size: 13px; }
.tile-value \{ font-size: 28px; font-weight: 700; letter-spacing: -0.02em; }
.tile-foot \{ justify-content: space-between; font-size: 12px; gap: 8px; }
.tile .spark \{ color: var(--accent); width: 80px; height: 24px; }
.charts \{ display: grid; grid-template-columns: 2fr 1fr; gap: 12px; }
.panel \{ background: var(--panel); border: 1px solid var(--line); border-radius: 10px; padding: 16px 18px; min-width: 0; }
.panel h2 \{ font-size: 14px; font-weight: 600; margin: 0 0 10px; }
.panel h2 + * \{ margin-top: 0; }
.table-tools \{ margin-bottom: 12px; gap: 8px; }
.search \{ min-width: 240px; flex: 1; max-width: 340px; }
.chip \{ padding: 4px 12px; border-radius: 999px; font-size: 13px; text-transform: capitalize; }
.chip.on \{ background: var(--ink); color: var(--bg); border-color: var(--ink); }
.grid \{ width: 100%; border-collapse: collapse; }
.grid th \{ text-align: left; font-weight: 500; color: var(--muted); font-size: 12.5px; padding: 6px 10px; border-bottom: 1px solid var(--line); cursor: pointer; user-select: none; white-space: nowrap; }
.grid th.sorted \{ color: var(--ink); }
.grid td \{ padding: 9px 10px; border-bottom: 1px solid var(--line); }
.grid .num \{ text-align: right; font-variant-numeric: tabular-nums; }
.grid td.name \{ font-weight: 600; }
.grid td.name .lamp \{ margin-right: 9px; }
.grid tr.pick \{ cursor: pointer; }
.grid tr.pick:hover td \{ background: color-mix(in srgb, var(--ink) 4%, transparent); }
.empty \{ padding: 18px 10px; }
.back \{ color: var(--muted); text-decoration: none; font-size: 13px; }
.back:hover \{ color: var(--ink); }
.svc-head \{ display: flex; align-items: baseline; gap: 12px; flex-wrap: wrap; }
.svc-head h1 \{ margin: 0; font-size: 32px; font-weight: 800; letter-spacing: -0.03em; }
.svc-head .lamp \{ width: 14px; height: 14px; align-self: center; }
.log \{ list-style: none; margin: 0 0 18px; padding: 0; display: flex; flex-direction: column; gap: 8px; }
.log li \{ display: grid; grid-template-columns: 48px 1fr auto; gap: 8px; }
.at \{ color: var(--muted); }
.incident \{ display: flex; flex-wrap: wrap; gap: 8px 12px; align-items: center; padding: 12px 4px; border-bottom: 1px solid var(--line); }
.incident .inc-body \{ flex: 1 1 240px; }
.incident:last-child \{ border-bottom: 0; }
.incident.acked .inc-title \{ color: var(--muted); }
.inc-body \{ display: flex; flex-direction: column; }
.inc-body a \{ color: var(--accent); }
.inc-title \{ font-weight: 600; }
.sev \{ min-width: 52px; text-align: center; font-size: 12px; font-weight: 600; padding: 2px 8px; border-radius: 6px; text-transform: capitalize;
  background: color-mix(in srgb, var(--muted) 14%, transparent); }
.sev.high \{ background: color-mix(in srgb, var(--down) 16%, transparent); color: var(--down); }
.sev.medium \{ background: color-mix(in srgb, var(--warn) 18%, transparent); color: var(--warn); }
.problem \{ max-width: 52ch; }
.push \{ margin-left: auto; }
.report \{ display: flex; flex-direction: column; gap: 14px; }
.report h2 \{ margin: 0; font-size: 20px; font-weight: 700; letter-spacing: -0.01em; }
.report .pair \{ flex-wrap: nowrap; align-items: start; }
.report .pair > * \{ flex: 1; }
.report .actions \{ justify-content: flex-end; margin-top: 4px; }
.field select, .field textarea, .field input \{ width: 100%; box-sizing: border-box; }
.log-head, .log-row \{ display: grid; grid-template-columns: 90px 140px 70px 70px 1fr; gap: 12px; align-items: center; padding: 0 10px; }
.log-head \{ color: var(--muted); font-size: 12.5px; padding-bottom: 6px; border-bottom: 1px solid var(--line); }
.log-row \{ height: 34px; border-bottom: 1px solid var(--line); font-variant-numeric: tabular-nums; }
.log-row .num, .log-head .num \{ text-align: right; }
.log-row .at \{ color: var(--muted); }
.log-row .path \{ overflow: hidden; text-overflow: ellipsis; white-space: nowrap; color: var(--muted); }
.log-row.bad .code \{ color: var(--down); font-weight: 600; }
.log-row.miss .code \{ color: var(--warn); }
#log-rows \{ height: min(62vh, 640px); }
@media (max-width: 860px) \{
  .app \{ grid-template-columns: 1fr; }
  .rail \{ flex-direction: row; flex-wrap: wrap; align-items: center; border-right: 0; border-bottom: 1px solid var(--line); padding: 10px 16px; }
  .brand \{ padding: 0 10px 0 0; }
  .rail-foot \{ margin: 0 0 0 auto; padding: 0; }
  .main \{ padding: 16px; }
  .charts \{ grid-template-columns: 1fr; }
  .grid \{ font-size: 13px; }
  .grid th:nth-child(2), .grid td:nth-child(2), .grid th:nth-child(3), .grid td:nth-child(3) \{ display: none; }
}
")

mount(App)
