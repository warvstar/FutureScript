// The hello-ui task list as views in blocks: one of the two view syntaxes on trial (the other: views-markup.fut).
//   node web/serve.mjs   then open http://localhost:8000/?file=examples/views-blocks.fut
import "std/ui"

class Task {
  state effort
  state done
  fn init(i) {
    self.serial = i + 1
    self.effort = i % 5 + 1
    self.done = i % 3 == 0
  }
}
fn more(e) { return e >= 5 ? 1 : e + 1 }
fn weight(e) { return e > 3 ? "heavy" : e > 1 ? "medium" : "light" }

state name = ""
state clicks = 0
state compact = false
var tasks = []
for i in 0..24 { tasks.push(Task(i)) }

view Row(t) {
  row .task .done: t.done .compact: compact {
    check "Task {t.serial}" bind: t.done
    text .effort "{weight(t.effort)} ({t.effort})"
    button "More effort" id: "more{t.serial}" on click { t.effort = more(t.effort) }
  }
}

view Page {
  column .page {
    text .title "Hello, fut"
    text .muted "This page is fut views over state."
    field "Your name" bind: name id: "name"
    if name.len() > 0 {
      text "Hi, {name}! You clicked {clicks} times."
    } else {
      text .muted "Type your name, then click."
    }
    row .bar {
      button .primary "Click me" id: "click" on click { clicks += 1 }
      check "Compact rows" bind: compact id: "compact"
    }
    column .list { for t in tasks key t.serial { Row(t) } }
  }
}

ui.style("
.page \{ padding: 24px; max-width: 688px; }
.list \{ gap: 0; }
.task \{ padding: 6px 4px; border-bottom: 1px solid var(--line); }
.effort \{ flex-grow: 1; }
.done \{ opacity: 0.5; }
.compact \{ padding: 0 4px; font-size: 12px; }
")
mount(Page)
