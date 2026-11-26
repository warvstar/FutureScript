// Units on hand per SKU, and how many of them open orders hold. The invariant holds after every call from outside a
// Stock; `fut test` also drives a Stock with random calls of its methods with contracts, checking it after each one.

pub class Stock {
  var counts = nil  // sku -> units on hand
  var held = nil    // sku -> units reserved by orders not shipped yet
  invariant self.consistent()

  fn init() {
    self.counts = {}
    self.held = {}
  }
  fn onHand(sku: str): num { return self.counts[sku] || 0 }
  fn heldFor(sku: str): num { return self.held[sku] || 0 }
  fn available(sku: str): num { return self.onHand(sku) - self.heldFor(sku) }

  fn add(sku: str, n: num)
    requires n >= 1
    requires n == floor(n)
    ensures self.onHand(sku) == old(self.onHand(sku)) + n
  {
    self.counts[sku] = self.onHand(sku) + n
  }
  fn reserve(sku: str, n: num)
    requires n >= 1
    requires n == floor(n)
    requires n <= self.available(sku)     // checking availability is the caller's job
    ensures self.available(sku) == old(self.available(sku)) - n
  {
    self.held[sku] = self.heldFor(sku) + n
  }
  fn release(sku: str, n: num)
    requires n >= 1
    requires n <= self.heldFor(sku)
    ensures self.available(sku) == old(self.available(sku)) + n
  {
    self.held[sku] = self.heldFor(sku) - n
  }
  fn ship(sku: str, n: num)
    requires n >= 1
    requires n <= self.heldFor(sku)
    ensures self.onHand(sku) == old(self.onHand(sku)) - n
    ensures self.available(sku) == old(self.available(sku))
  {
    self.counts[sku] = self.onHand(sku) - n
    self.held[sku] = self.heldFor(sku) - n
  }

  // Nothing held that isn't on hand, and no negative counts.
  fn consistent(): bool {
    for k in self.counts { if self.counts[k] < 0 { return false } }
    for k in self.held { if self.held[k] < 0 || self.held[k] > self.onHand(k) { return false } }
    return true
  }
}
