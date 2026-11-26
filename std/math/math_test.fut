import "./math.fut"

test "libm" {
  expect pow(2, 10) == 1024 && sin(0) == 0 && cos(0) == 1 && exp(0) == 1 && log(1) == 0
  expect log2(8) == 3 && log10(1000) == 3 && cbrt(27) == 3 && hypot(3, 4) == 5
  expect abs(atan2(1, 1) - pi / 4) < 1e-12
  expect ceil(1.2) == 2 && trunc(-1.7) == -1
}
test "helpers" {
  expect abs(-3) == 3 && min(1, 2) == 1 && max(1, 2) == 2
  expect clamp(5, 0, 3) == 3 && clamp(-1, 0, 3) == 0
  expect sign(-2) == -1 && sign(0) == 0
  expect round(2.5) == 3 && round(-2.5) == -3 && round(1.4) == 1
  expect isNaN(0 / 0) && !isNaN(1)
}
fn pure(): num ! { return pow(2, 2) + abs(-1) + round(0.4) }
test "callable from code that performs nothing" {
  expect pure() == 5
}
