// A small store: three packages (money, stock, orders) whose functions and classes state their contracts. Run it with
// `fut store.fut`; test it with `fut test store_test.fut`, which also checks every contract on random inputs; `fut map
// store.fut` shows the layout, and each package's .api its interface, contracts included.
import "money"
import "stock"
import "orders"

fn cart(items, code) {
  var c = orders.Cart()
  for it in items { c.add(it[0], it[1], it[2]) }
  c.code = code
  return c
}

fn main() {
  var st = stock.Stock()
  st.add("apple", 10)
  st.add("pear", 4)
  st.add("plum", 1)
  var carts = [
    cart([["apple", 3, 120], ["pear", 2, 250]], nil),
    cart([["plum", 1, 900], ["pear", 5, 250]], "SAVE10"),   // not enough pears: the plum stays available
    cart([["plum", 1, 900], ["apple", 7, 1500]], "BULK"),
    cart([], nil)
  ]
  var events = []
  handle {
    for c in carts {
      try {
        var o = orders.checkout(c, st)
        print("ok   " + money.format(o.total))
      } catch e {
        print("no   " + e)
      }
    }
  } with orders.Audit.record(e) { events.push(e) }
  for e in events { print("audit: " + e) }
  for sku in ["apple", "pear", "plum"] { print(sku + ": " + str(st.available(sku)) + " available of " + str(st.onHand(sku))) }
  print("split three ways: " + str(money.split(1000, 3)))
}
