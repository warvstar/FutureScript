// Players, monsters and items, and the numbers that grow with level.

pub class Item {
  var name: str
  var kind: str      // "weapon", "armor", "potion" or "junk"
  var power: num     // attack for a weapon, defense for armor, hp healed for a potion
  var weight: num
  var value: num     // what a shop asks for it, in gold
  fn init(name: str, kind: str, power: num, weight: num, value: num) {
    self.name = name self.kind = kind self.power = power self.weight = weight self.value = value
  }
}

pub class Player {
  var name: str
  var hp: num = 30
  var maxHp: num = 30
  var atk: num = 5
  var def: num = 2
  var speed: num = 5
  var gold: num = 0
  var xp: num = 0
  var level: num = 1
  var inventory: list = []
  var weapon: Item? = nil
  var armor: Item? = nil
  fn init(name: str) { self.name = name }
}

pub class Monster {
  var name: str
  var hp: num
  var maxHp: num
  var atk: num
  var def: num
  var speed: num = 4
  var xpReward: num = 0
  var goldReward: num = 0
  fn init(name: str, maxHp: num, atk: num, def: num) {
    self.name = name self.maxHp = maxHp self.hp = maxHp self.atk = atk self.def = def
  }
}

// A player or monster that still stands.
pub fn isAlive(c: any): bool ! { return c.hp > 0 }

// Total xp a player needs to reach a level: 0 for level 1, then 10, 40, 90, ...
pub fn xpForLevel(level: num): num ! { return (level - 1) * (level - 1) * 10 }

// Adds xp, levelling the player up each time their total reaches xpForLevel(level + 1).
// Each level adds 5 max hp, 2 atk and 1 def; any level gained heals to full.
// Returns how many levels were gained.
pub fn gainXp(p: Player, amount: num): num ! {
  p.xp += amount
  var gained = 0
  while p.xp >= xpForLevel(p.level + 1) {
    p.level += 1
    p.maxHp += 5
    p.atk += 2
    p.def += 1
    gained += 1
  }
  if gained > 0 { p.hp = p.maxHp }
  return gained
}

// Attack including the equipped weapon.
pub fn attackPower(p: Player): num ! {
  if p.weapon == nil { return p.atk }
  return p.atk + p.weapon.power
}

// Defense including the equipped armor.
pub fn defensePower(p: Player): num ! {
  if p.armor == nil { return p.def }
  return p.def + p.armor.power
}

var templates = {
  slime: {hp: 8, atk: 3, def: 0, speed: 2, xp: 4, gold: 2},
  goblin: {hp: 12, atk: 5, def: 1, speed: 6, xp: 8, gold: 5},
  wolf: {hp: 14, atk: 6, def: 1, speed: 8, xp: 10, gold: 3},
  skeleton: {hp: 18, atk: 7, def: 3, speed: 3, xp: 14, gold: 8}
}

// A monster of a known kind. Each level above 1 adds 20% to its hp, atk and rewards (rounded down).
pub fn makeMonster(kind: str, level: num): Monster ! {
  var t = templates[kind]
  if t == nil { throw "unknown monster '{kind}'" }
  var scale = 1 + (level - 1) * 0.2
  var m = Monster(kind, floor(t.hp * scale), floor(t.atk * scale), t.def)
  m.speed = t.speed
  m.xpReward = floor(t.xp * scale)
  m.goldReward = floor(t.gold * scale)
  return m
}

// A named boss for the end of a floor: slow, tough, and carrying 40 gold per level.
pub fn makeBoss(name: str, level: num): Monster ! {
  var m = Monster(name, 30 + level * 15, 6 + level * 2, 2 + level)
  m.speed = 3
  m.xpReward = level * 25
  m.goldReward = level * 40
  return m
}

test "a new player starts at level 1 with full hp" {
  var p = Player("Ana")
  expect p.level == 1 && p.hp == p.maxHp && p.gold == 0
  expect isAlive(p)
  p.hp = 0
  expect !isAlive(p)
}

test "xp needed per level" {
  expect xpForLevel(1) == 0 && xpForLevel(2) == 10
  expect xpForLevel(4) == 90
}

test "equipment adds to attack and defense" {
  var p = Player("Ana")
  expect attackPower(p) == 5 && defensePower(p) == 2
  p.weapon = Item("Axe", "weapon", 4, 5, 30)
  p.armor = Item("Mail", "armor", 3, 8, 50)
  expect attackPower(p) == 9 && defensePower(p) == 5
}

test "monsters scale with level" {
  var g = makeMonster("goblin", 1)
  expect g.hp == 12 && g.atk == 5 && g.speed == 6 && g.xpReward == 8
  var g3 = makeMonster("goblin", 3)
  expect g3.maxHp == 16 && g3.atk == 7 && g3.goldReward == 7
  expect throws { makeMonster("dragon", 1) } == "unknown monster 'dragon'"
}
