fn pixel(cx, cy) {
  var x = 0.0
  var y = 0.0
  var i = 0
  while i < 200 {
    var x2 = x * x
    var y2 = y * y
    if x2 + y2 > 4 { return i }
    y = 2 * x * y + cy
    x = x2 - y2 + cx
    i += 1
  }
  return i
}
fn kernel() {
  var size = 1000
  var total = 0
  for py in 0..size {
    for px in 0..size { total += pixel(px * 3.0 / size - 2, py * 3.0 / size - 1.5) }
  }
  return total
}
