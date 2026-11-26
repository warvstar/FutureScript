// An isolate fails and nothing joins it: the program reports it when it ends, instead of losing it.
fn main() {
  spawn fn() { throw "lost" }()
  print("main done")
}
