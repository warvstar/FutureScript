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
  expect mix(2, 4, 0.25) == 2.5
  expect mix(vec2f(0, 2), vec2f(4, 6), 0.5) == vec2f(2, 4)
  expect mix(vec2f(0, 2), vec2f(4, 6), vec2f(0, 1)) == vec2f(0, 6)
  expect fract(2.25) == 0.25 && fract(-0.25) == 0.75 && fract(3) == 0
  expect fract(vec2f(1.5, -0.25)) == vec2f(0.5, 0.75)
  expect smoothstep(0, 2, -1) == 0 && smoothstep(0, 2, 1) == 0.5 && smoothstep(0, 2, 3) == 1
}
fn pure(): num ! { return pow(2, 2) + abs(-1) + round(0.4) }
test "callable from code that performs nothing" {
  expect pure() == 5
}
