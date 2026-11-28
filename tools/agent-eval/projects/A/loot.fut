// Drops, the bag, equipment and gold.

import "entities.fut"

// A small healing potion.
pub fn potion(): Item ! { return Item("Potion", "potion", 10, 1, 15) }

// What a kind of monster can drop: a list of {item, chance}, chance in [0, 1].
pub fn dropTable(kind: str): list ! {
  if kind == "slime" { return [{item: potion(), chance: 0.3}, {item: Item("Slime Gel", "junk", 0, 1, 4), chance: 0.6}] }
  if kind == "goblin" { return [{item: Item("Rusty Dagger", "weapon", 2, 2, 12), chance: 0.2}, {item: potion(), chance: 0.25}] }
  if kind == "wolf" { return [{item: Item("Wolf Pelt", "junk", 0, 3, 10), chance: 0.7}] }
  if kind == "skeleton" {
    return [{item: Item("Bone Shield", "armor", 2, 6, 25), chance: 0.15}, {item: Item("Old Sword", "weapon", 3, 5, 30), chance: 0.1}]
  }
  return []
}

// Each drop whose roll comes in under its chance, in table order.
pub fn rollDrops(table: list): list ! random {
  var out = []
  for d in table {
    if random() < d.chance { out.push(d.item) }
  }
  return out
}

// How much weight the player can carry: 20, plus 5 per level.
pub fn carryLimit(p: Player): num ! { return 20 + p.level * 5 }

// The weight of everything in the bag (equipped items don't count).
pub fn bagWeight(p: Player): num ! {
  var w = 0
  for it in p.inventory { w += it.weight }
  return w
}

// Puts it in the bag if the weight allows. Returns whether it went in.
pub fn addItem(p: Player, it: Item): bool ! {
  if bagWeight(p) + it.weight > carryLimit(p) { return false }
  p.inventory.push(it)
  return true
}

// Where it sits in the bag (this very item, not an equal one), or -1.
pub fn findItem(p: Player, it: Item): num ! {
  for i in 0..p.inventory.len() {
    if p.inventory[i] == it { return i }
  }
  return -1
}

// Takes the item at index i out of the bag.
fn takeAt(p: Player, i: num) ! {
  p.inventory = p.inventory.slice(0, i) + p.inventory.slice(i + 1, p.inventory.len())
}

// Takes it out of the bag. Returns whether it was there.
pub fn removeItem(p: Player, it: Item): bool ! {
  var i = findItem(p, it)
  if i < 0 { return false }
  takeAt(p, i)
  return true
}

// Moves a weapon or armor from the bag into its slot. Whatever was in the slot goes back in the bag
// and is returned (nil when the slot was empty).
pub fn equip(p: Player, it: Item): Item? ! {
  if it.kind != "weapon" && it.kind != "armor" { throw "can't equip {it.name}" }
  if !removeItem(p, it) { throw "{it.name} is not in the bag" }
  var old = nil
  if it.kind == "weapon" {
    old = p.weapon
    p.weapon = it
  } else {
    old = p.armor
    p.armor = it
  }
  if old != nil { p.inventory.push(old) }
  return old
}

// Sells it out of the bag for half its value, rounded down. Returns the gold paid, 0 when it isn't in the bag.
pub fn sellItem(p: Player, it: Item): num ! {
  var i = findItem(p, it)
  if i < 0 { return 0 }
  takeAt(p, i)
  var paid = floor(it.value / 2)
  p.gold += paid
  return paid
}

test "drop tables" {
  expect dropTable("wolf").len() == 1
  expect dropTable("slime")[0].item.name == "Potion"
  expect dropTable("dragon") == []
}

test "drops follow their chances" {
  var sure = [{item: potion(), chance: 1}, {item: Item("Gem", "junk", 0, 1, 99), chance: 0}]
  seed(11)
  var got = rollDrops(sure)
  expect got.len() == 1 && got[0].name == "Potion"
}

test "the bag has a weight limit" {
  var p = Player("Ana")
  expect carryLimit(p) == 25
  expect addItem(p, Item("Anvil", "junk", 0, 20, 5))
  expect bagWeight(p) == 20
  expect !addItem(p, Item("Mail", "armor", 3, 8, 50))
  expect addItem(p, potion())
  expect p.inventory.len() == 2
}

test "remove finds the very item" {
  var p = Player("Ana")
  var a = potion()
  var b = potion()
  addItem(p, a)
  expect findItem(p, b) == -1
  addItem(p, b)
  expect findItem(p, b) == 1
  expect removeItem(p, a)
  expect !removeItem(p, a)
  expect p.inventory.len() == 1
}

test "equip swaps with the slot" {
  var p = Player("Ana")
  var dagger = Item("Dagger", "weapon", 2, 2, 12)
  var sword = Item("Sword", "weapon", 4, 5, 40)
  addItem(p, dagger)
  addItem(p, sword)
  expect equip(p, dagger) == nil
  expect p.weapon == dagger && p.inventory.len() == 1
  expect equip(p, sword) == dagger
  expect p.weapon == sword && p.inventory[0] == dagger
  expect throws { equip(p, potion()) } == "can't equip Potion"
}
