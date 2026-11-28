// Discount codes, sales tax by region and shipping by weight. All amounts are whole cents.

pub enum Discount {
  Percent(pct: num)
  Fixed(cents: num)
  FreeShipping
}

var codes = {
  WELCOME10: Discount.Percent(10),
  SPRING25: Discount.Percent(25),
  FIVEOFF: Discount.Fixed(500),
  SHIPFREE: Discount.FreeShipping
}

// Tax rate per region; regions not listed pay none.
var taxRates = {CA: 0.0725, NY: 0.04, TX: 0.0625, WA: 0.065}

// Round half up to whole cents.
fn roundCents(x: num): num ! {
  return floor(x + 0.5)
}

// The discount for a code (case-insensitive), or nil when the code is unknown.
pub fn lookupCode(code: str): Discount? ! {
  var key = ""
  for c in code.trim() {
    var b = c.byte(0)
    key += b >= 97 && b <= 122 ? char(b - 32) : c
  }
  return codes[key]
}

// How much a discount takes off a subtotal. Never more than the subtotal.
pub fn discountCents(d: Discount, subtotal: num): num ! {
  match d {
    Percent(pct) { return roundCents(subtotal * pct / 100) }
    Fixed(cents) { return cents < subtotal ? cents : subtotal }
    FreeShipping { return 0 }
  }
}

pub fn taxRate(region: str): num ! {
  var r = taxRates[region]
  return r == nil ? 0 : r
}

pub fn taxCents(amount: num, region: str): num ! {
  return roundCents(amount * taxRate(region))
}

// Domestic: up to 500 g 499, up to 2 kg 899, then 250 more per started kg.
// "INTL" adds a flat 1500. An empty parcel ships free.
pub fn shippingCents(grams: num, region: str): num ! {
  if grams <= 0 { return 0 }
  var cents = 899
  if grams <= 500 {
    cents = 499
  } else if grams > 2000 {
    cents += int((grams - 2000 + 999) / 1000) * 250
  }
  if region == "INTL" { cents += 1500 }
  return cents
}

pub class ShippingQuote {
  var region: str
  var grams: num
  var costCents: num = 0
  var days: num = 0
  fn init(region: str, grams: num) {
    self.region = region
    self.grams = grams
  }
}

// Cost and transit time: 3 days domestic, 10 international, one more for parcels over 5 kg.
pub fn quoteShipping(grams: num, region: str): ShippingQuote ! {
  var q = ShippingQuote(region, grams)
  q.costCents = shippingCents(grams, region)
  q.days = region == "INTL" ? 10 : 3
  if grams > 5000 { q.days += 1 }
  return q
}

test "discount codes" {
  expect lookupCode("welcome10") == Discount.Percent(10)
  expect lookupCode(" FiveOff ") == Discount.Fixed(500)
  expect lookupCode("BOGUS") == nil
  expect discountCents(Discount.Percent(10), 4995) == 500
  expect discountCents(Discount.Fixed(500), 300) == 300
  expect discountCents(Discount.FreeShipping, 300) == 0
}

test "tax and shipping" {
  expect taxCents(10000, "CA") == 725
  expect taxCents(10000, "OR") == 0
  expect shippingCents(0, "CA") == 0
  expect shippingCents(400, "CA") == 499
  expect shippingCents(1500, "NY") == 899
  expect shippingCents(2001, "NY") == 1149
  expect shippingCents(400, "INTL") == 1999
}
