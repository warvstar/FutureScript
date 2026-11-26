// Three isolates send their letters into one channel: the order they arrive in is the interleaving.
fn letters(c, l) ! chan {
  for i in 0..4 { c.send(l) }
}
fn main() {
  var c = channel(0)
  var ts = [spawn letters(c, "a"), spawn letters(c, "b"), spawn letters(c, "c")]
  var s = ""
  for i in 0..12 { s += c.recv() }
  for t in ts { t.join() }
  print(s)
}
