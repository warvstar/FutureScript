// mandelbrot.fut's kernel over THREADS isolates (run.py sets it): each takes every THREADS-th row, like the C
// version's threads, and join() brings back its sum.
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
fn rows(t, n, size) {
  var total = 0
  for py in 0..size {
    if py % n == t {
      for px in 0..size { total += pixel(px * 3.0 / size - 2, py * 3.0 / size - 1.5) }
    }
  }
  return total
}
fn kernel() {
  var tasks = []
  for t in 0..THREADS { tasks.push(spawn rows(t, THREADS, 1000)) }
  var total = 0
  for k in tasks { total += k.join() }
  return total
}
