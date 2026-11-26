fn kernel() {
  var m = {}
  for i in 0..2000000 {
    var k = "key" + str((i * 7) % 100000)
    var c = m[k]
    if c == nil { m[k] = 1 } else { m[k] = c + 1 }
  }
  return len(m) * 1000 + m["key14"]
}
