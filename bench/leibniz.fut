fn kernel() {
  var s = 0.0
  var sign = 1.0
  for i in 0..50000000 {
    s += sign / (2 * i + 1)
    sign = -sign
  }
  return s * 4
}
