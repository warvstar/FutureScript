// The error codes of isolates and channels (see codes.fut), which wasm lacks yet.
fn err(src) { return run({source: src}).err }

test "isolates and channels" {
  expect err("var c = channel(1)\nc.send(fiber(fn() \{ }))").has(" [send]\n")
  expect err("var c = channel(1)\nc.close()\nc.close()").has(" [channel]\n")
  expect err("spawn print(1)").has(" [spawn]\n")
}
