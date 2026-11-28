// What the shop sells: products, their variants, and how many of each are on the shelf.

pub class Variant {
  var sku: str
  var name: str
  var priceCents: num
  var stock: num
  var weightGrams: num
  fn init(sku: str, name: str, priceCents: num, stock: num, weightGrams: num) {
    self.sku = sku
    self.name = name
    self.priceCents = priceCents
    self.stock = stock
    self.weightGrams = weightGrams
  }
  fn inStock(): bool { return self.stock > 0 }
}

pub class Product {
  var id: str
  var name: str
  var variants: list<Variant> = []
  fn init(id: str, name: str) {
    self.id = id
    self.name = name
  }
  fn add(v: Variant): Product {
    self.variants.push(v)
    return self
  }
}

pub class Catalog {
  var products: list<Product> = []
  fn add(p: Product): Catalog {
    self.products.push(p)
    return self
  }
}

// The variant with this SKU, or nil.
pub fn findVariant(cat: Catalog, sku: str): Variant? ! {
  for p in cat.products {
    for v in p.variants {
      if v.sku == sku { return v }
    }
  }
  return nil
}

// Can the shelf cover an order of qty units? Exactly enough counts.
pub fn canFulfil(v: Variant, qty: num): bool ! {
  return v.stock >= qty
}

// Take qty units off the shelf; throws when there aren't enough.
pub fn takeStock(v: Variant, qty: num) ! {
  if !canFulfil(v, qty) { throw "only {v.stock} of {v.sku} left" }
  v.stock -= qty
}

pub fn restock(v: Variant, qty: num) ! {
  if qty <= 0 { throw "restock needs a positive quantity" }
  v.stock += qty
}

// [lowest, highest] variant price of a product, in cents.
pub fn priceRange(p: Product): list<num> ! {
  if p.variants.len() == 0 { return [0, 0] }
  var lo = p.variants[0].priceCents
  var hi = lo
  for v in p.variants {
    if v.priceCents < lo { lo = v.priceCents }
    if v.priceCents > hi { hi = v.priceCents }
  }
  return [lo, hi]
}

test "catalog lookups" {
  var mug = Product("mug", "Enamel mug").add(Variant("MUG-BLU", "Blue", 1200, 5, 350)).add(Variant("MUG-RED", "Red", 1400, 0, 350))
  var cat = Catalog().add(mug)
  expect findVariant(cat, "MUG-RED").priceCents == 1400
  expect findVariant(cat, "NOPE") == nil
  expect priceRange(mug) == [1200, 1400]
  expect !findVariant(cat, "MUG-RED").inStock()
}
