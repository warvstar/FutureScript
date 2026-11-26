import "./reactive.fut"

test "an effect reruns when a signal it read changes" {
  var n = signal(1)
  var seen = []
  effect(fn() { seen.push(n.get()) })
  n.set(2)
  n.set(2)  // the same value: nothing reruns
  n.update(fn(v) { return v * 10 })
  expect seen == [1, 2, 20]
}

test "dependencies are what the last run read" {
  var on = signal(true)
  var a = signal("a")
  var b = signal("b")
  var seen = []
  effect(fn() { if on.get() { seen.push(a.get()) } else { seen.push(b.get()) } })
  b.set("b2")      // not read: no rerun
  on.set(false)    // now reads b
  a.set("a2")      // no longer read
  b.set("b3")
  expect seen == ["a", "b2", "b3"]
}

test "computed, batch, untrack, peek" {
  var first = signal("Ada")
  var last = signal("L")
  var full = computed(fn() { return first.get() + " " + last.get() })
  var seen = []
  effect(fn() { seen.push(full.get()) })
  batch(fn() { first.set("Grace") last.set("H") })
  expect seen == ["Ada L", "Grace H"]
  var c = signal(0)
  var runs = []
  effect(fn() { runs.push(untrack(fn() { return c.get() }) + c.peek()) })
  c.set(5)
  expect runs == [0]
}

test "effects made inside an effect belong to it" {
  var show = signal(true)
  var x = signal(1)
  var inner = []
  var cleaned = []
  effect(fn() {
    if show.get() {
      effect(fn() { inner.push(x.get()) onCleanup(fn() { cleaned.push(1) }) })
    }
  })
  x.set(2)
  show.set(false)  // the inner effect is disposed: x no longer reaches it
  x.set(3)
  expect inner == [1, 2]
  expect cleaned.len() == 2
}

test "root owns until disposed; dispose stops an effect" {
  var n = signal(0)
  var seen = []
  var dispose = root(fn() { effect(fn() { seen.push(n.get()) }) })
  n.set(1)
  dispose()
  n.set(2)
  var e = effect(fn() { seen.push(n.get() * 100) })
  e.dispose()
  n.set(3)
  expect seen == [0, 1, 200]
}

test "an effect that keeps changing what it reads stops" {
  var n = signal(0)
  expect throws { effect(fn() { n.set(n.get() + 1) }) } == "effects keep changing the signals they read"
}

test "an effect that throws leaves the rest working" {
  var n = signal(0)
  var seen = []
  effect(fn() { if n.get() == 1 { throw "boom" } seen.push(n.get()) })
  expect throws { n.set(1) } == "boom"
  var m = signal("a")
  effect(fn() { seen.push(m.get()) })
  m.set("b")
  n.set(2)
  expect seen == [0, "a", "b", 2]
  expect throws { batch(fn() { n.set(1) }) } == "boom"
  m.set("c")
  expect seen == [0, "a", "b", 2, "c"]
}
