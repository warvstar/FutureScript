// Reactive state: a signal holds a value; an effect runs a function now and again whenever a signal it read changes.
// What an effect depends on is whatever it read on its last run, found as it runs (nothing is declared), so a
// branch not taken is no dependency. std/ui builds pages on it: each piece of the page that shows a signal is an
// effect that updates just that piece.
//
// Effects made while another effect runs belong to it: they are disposed before it runs again, or when it is
// disposed, so conditional and repeated parts of a page don't leak. root(f) makes an owner that doesn't rerun and
// that only its own dispose function ends.

var current = nil   // the effect (or root) running now: what signals read now subscribe, and what owns new effects
var tracking = true
var pending = []    // effects to run, in the order their signals changed
var batching = 0
var flushing = false
var nextId = 0

// A value that effects can depend on. get() reads it (and subscribes the running effect); set() changes it and reruns
// the effects that read it, unless the new value == the old.
pub class Signal {
  var value
  var subs  // effect id -> effect
  fn init(v) { self.value = v self.subs = {} }
  fn get() {
    if current != nil && tracking && current.f != nil && !self.subs.has(current.id) {
      self.subs[current.id] = current
      current.deps.push(self)
    }
    return self.value
  }
  fn peek() { return self.value }  // without subscribing
  fn set(v) {
    if v == self.value { return }
    self.value = v
    for e in self.subs.values() { schedule(e) }
    if batching == 0 { flush() }
  }
  fn update(f: fn) { self.set(f(self.value)) }
}

// An effect, or a root (f == nil): it owns the effects made while it runs, and runs its cleanups before it reruns.
class Effect {
  var id
  var f
  var deps
  var children
  var cleanups
  var queued = false
  var alive = true
  fn init(id, f) { self.id = id self.f = f self.deps = [] self.children = [] self.cleanups = [] }
  fn run() {
    if !self.alive || self.f == nil { return }
    self.clear()
    var prev = current
    var t = tracking
    current = self
    tracking = true
    var f = self.f
    try { f() } catch e { current = prev tracking = t throw e }
    current = prev
    tracking = t
  }
  fn clear() {
    for c in self.children { c.dispose() }
    self.children = []
    for c in self.cleanups { c() }
    self.cleanups = []
    for s in self.deps { s.subs.remove(self.id) }
    self.deps = []
  }
  fn dispose() {
    self.clear()
    self.alive = false
  }
}

fn owned(f) {
  nextId += 1
  var e = Effect(nextId, f)
  if current != nil { current.children.push(e) }
  return e
}
fn schedule(e) {
  if e.queued || !e.alive { return }
  e.queued = true
  pending.push(e)
}
fn flush() {  // run what is pending, and what that makes pending, once (an effect's own set() doesn't nest a flush)
  if flushing { return }
  flushing = true
  var rounds = 0
  try {
    while pending.len() > 0 {
      rounds += 1
      if rounds > 10000 { throw "effects keep changing the signals they read" }
      var q = pending
      pending = []
      for e in q {
        e.queued = false
        e.run()
      }
    }
  } catch err {
    for e in pending { e.queued = false }
    pending = []
    flushing = false
    throw err
  }
  flushing = false
}

pub fn signal(v): Signal {
  return Signal(v)
}

// For `state` fields (src/views.cpp): a field of that name read or set on something that may not be a class
// declaring it. val(x) is x's value if x is a signal, else x; store(old, v) sets old if it is a signal (and keeps it
// in the field), else v replaces it.
pub fn val(x) {
  if isa(x, Signal) { return x.get() }
  return x
}
pub fn cell(v): Signal {  // `state x = e`: e's signal if it is one (computed(), ui.persist()), else a new one
  if isa(v, Signal) { return v }
  return Signal(v)
}
// `state x = e` at the top level: as cell(e), under its name, for an inspector (watch()).
var namedStates = {}  // name -> its signal
var watcher = nil     // fn(name, signal, gone), or nil
pub fn named(name: str, v): Signal {
  var s = cell(v)
  namedStates[name] = s
  if watcher != nil { watcher(name, s, false) }
  return s
}
// f(name, signal, false) for each named state, now and as more are made (a view's own: "View.x", then "View.x#2"
// for the second made since the page was shown, and so on), and f(name, signal, true) when one goes (its view did).
pub fn watch(f: fn) {
  watcher = f
  for k in namedStates.keys() { f(k, namedStates[k], false) }
  for k in localsMade.keys() {
    for e in localsMade[k] { if e.live { f(e.name, e.s, false) } }
  }
}

// `state x = e` in a function or view: as cell(e), but after a reload (keepLocals()) the key's states take the values
// those of the page before had, in the order they were made.
var localsMade = {}  // key -> [{s, live}], in the order made
var localsKept = {}  // key -> the values before the reload
var localsTaken = {}  // key -> how many of them are taken
var localsCount = {}  // key -> how many made since the page was shown (for their names)
pub fn local(key: str, v): Signal {
  if isa(v, Signal) { return v }
  var s = Signal(v)
  if localsKept.has(key) {
    var i = 0
    if localsTaken.has(key) { i = localsTaken[key] }
    if i < localsKept[key].len() { s.value = localsKept[key][i] }
    localsTaken[key] = i + 1
  }
  if !localsMade.has(key) { localsMade[key] = [] }
  var list = localsMade[key]
  if list.len() >= 64 && list.len() % 64 == 0 {  // (rows come and go: forget those gone)
    var live = []
    for e in list { if e.live { live.push(e) } }
    list = live
    localsMade[key] = live
  }
  var n = 1
  if localsCount.has(key) { n = localsCount[key] + 1 }
  localsCount[key] = n
  var name = key
  if n > 1 { name = "{key}#{n}" }
  var entry = {s: s, live: true, name: name}
  list.push(entry)
  if watcher != nil { watcher(name, s, false) }
  onCleanup(fn() {
    entry.live = false
    if watcher != nil { watcher(name, s, true) }
  })
  return s
}
// Before a page is shown again (a reload): what its own states hold now, for local() to give the next page's.
pub fn keepLocals() {
  localsKept = {}
  for k in localsMade.keys() {
    var vals = []
    for e in localsMade[k] { if e.live { vals.push(e.s.value) } }
    localsKept[k] = vals
  }
  localsMade = {}
  localsTaken = {}
  localsCount = {}
}
pub fn store(old, v) {
  if isa(old, Signal) { old.set(v) return old }
  return v
}

// Run f now, and again whenever a signal it read last time changes. dispose() on the result stops it.
pub fn effect(f: fn) {
  var e = owned(f)
  batch(fn() { e.run() })
  return e
}

// A signal that holds f(), recomputed when what f read changes.
pub fn computed(f: fn): Signal {
  var s = Signal(nil)
  effect(fn() { s.set(f()) })
  return s
}

// f(), with the effects that its changes rerun run once, after it.
pub fn batch(f: fn) {
  batching += 1
  try { f() } catch e { batching -= 1 throw e }
  batching -= 1
  if batching == 0 { flush() }
}

// f(), reading signals without subscribing the running effect to them.
pub fn untrack(f: fn) {
  var t = tracking
  tracking = false
  try {
    var r = f()
    tracking = t
    return r
  } catch e { tracking = t throw e }
}

// Run f before the running effect reruns or is disposed (to undo what it did outside signals).
pub fn onCleanup(f: fn) {
  if current != nil { current.cleanups.push(f) }
}

// f() as an owner of the effects it makes, which live until the returned function is called (not when the effect
// running now reruns: a root belongs to no one).
pub fn root(f: fn): fn {
  var prev = current
  current = nil  // its own owner: not disposed with the effect running now
  var r = owned(nil)
  current = r
  try { f() } catch e { current = prev throw e }
  current = prev
  return fn() { r.dispose() }
}
