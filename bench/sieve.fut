fn kernel() {
  var n = 10000000
  var flags = []
  for i in 0..n { flags.push(true) }
  var count = 0
  for i in 2..n {
    if flags[i] {
      count += 1
      var j = i * i
      while j < n { flags[j] = false j += i }
    }
  }
  return count
}
