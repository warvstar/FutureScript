var x = 3
var name = "fut"
print("{name} has {x + 1} things")      // expect: fut has 4 things
print("{x}")                            // expect: 3
print("nested {{1: 2}}")                // expect: nested {1: 2}
print("escaped \{x}")                   // expect: escaped {x}
print("list {[1, 2]} map {len({a: 1})}") // expect: list [1, 2] map 1
