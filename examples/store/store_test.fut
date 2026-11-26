// `fut test store_test.fut`: these tests, plus a property test for every contract in the three packages (their functions
// on random inputs, Stock and Cart under random calls).
import "money"
import "stock"
import "orders"

fn stocked() {
  var st = stock.Stock()
  st.add("apple", 5)
  st.add("pear", 1)
  return st
}
fn cartOf(n) {  // n lines of one apple at $1.50
  var c = orders.Cart()
  for i in 0..n { c.add("apple", 1, 150) }
  return c
}

test "money formats cents and splits them exactly" {
  expect money.format(123456) == "$1234.56"
  expect money.format(-5) == "-$0.05"
  expect money.cents(19.999) == 2000
  expect money.split(1000, 3) == [334, 333, 333]
}

test "checkout reserves stock and reports the order through Audit" {
  var st = stocked()
  var c = cartOf(2)
  var events = []
  handle {
    var o = orders.checkout(c, st)
    expect o == {total: 300, lines: 2}
  } with orders.Audit.record(e) { events.push(e) }
  expect events == ["order $3.00, 2 lines"]
  expect st.available("apple") == 3
}

test "a checkout that fails reserves nothing" {
  var st = stocked()
  var c = orders.Cart()
  c.add("apple", 2, 150)
  c.add("pear", 3, 250)   // only one pear
  handle {
    expect throws { orders.checkout(c, st) } == "out of stock: pear"
  } with orders.Audit.record(e) { }
  expect st.available("apple") == 5   // the apples reserved before the pears were put back
}

test "discount codes" {
  var c = cartOf(100)          // $150.00
  c.code = "BULK"
  expect orders.total(c) == 14250
  c.code = "SAVE10"
  expect orders.total(c) == 13500
  c.code = "save10"
  expect orders.total(c) == 15000
}

test "contracts blame the caller for a bad quantity" {
  var c = orders.Cart()
  expect throws { c.add("apple", 1.5, 150) } == "the caller broke orders.Cart.add()'s contract: requires qty == floor(qty) (qty is 1.5, floor(qty) is 1)"
  var st = stocked()
  expect throws { st.reserve("pear", 2) } == "the caller broke stock.Stock.reserve()'s contract: requires n <= self.available(sku) (n is 2, self.available(sku) is 1)"
}

test "checkout's cost" {
  var st = stocked()
  var c = cartOf(3)
  expect allocs { orders.total(c) } == 0
  expect grows { n -> orders.total(cartOf(n)) } like "n"    // building the cart included
  expect grows { n -> money.split(n * 100, n) } like "n"
}

test "the program's output" {
  var r = run("store.fut")
  expect r.code == 0
  expect r.out == "ok   $8.60\nno   out of stock: pear\nok   $108.30\nno   empty cart\naudit: order $8.60, 2 lines\naudit: order $108.30, 2 lines\napple: 0 available of 10\npear: 2 available of 4\nplum: 0 available of 1\nsplit three ways: [334, 333, 333]\n"
}
