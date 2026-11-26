// main waits for an isolate that waits for main: neither can go on.
fn main() {
  var c = channel(0)
  var t = spawn fn(c) { c.recv() }(c)
  t.join()
}
