// Hits, crits, and who acts when.

import "entities.fut"
import "std/math"
import "std/lists"

// Damage before luck: attack minus defense, never below 1.
pub fn baseDamage(atk: num, def: num): num ! { return math.max(1, atk - def) }

// Crits double damage. roll is in [0, 1); it crits when below critChance.
pub fn critMultiplier(roll: num, critChance: num): num ! {
  if roll < critChance { return 2 }
  return 1
}

// One hit: base damage, a 10% chance to crit, then a spread of -1, 0 or +1 (at least 1).
pub fn rollDamage(atk: num, def: num): num ! random {
  var dmg = baseDamage(atk, def) * critMultiplier(random(), 0.1)
  var spread = floor(random() * 3) - 1
  return math.max(1, dmg + spread)
}

// Lowers a player's or monster's hp by amount, not below 0. Returns the hp it actually lost.
pub fn applyDamage(target: any, amount: num): num ! {
  var lost = math.min(target.hp, amount)
  target.hp -= lost
  return lost
}

// The monsters still standing, in order.
pub fn livingMonsters(ms: list): list ! {
  var out = []
  for m in ms {
    if isAlive(m) { out.push(m) }
  }
  return out
}

// Over when the player is down or no monster stands.
pub fn battleOver(p: Player, ms: list): bool ! {
  return !isAlive(p) || livingMonsters(ms).len() == 0
}

// Who acts this round, fastest first. The player wins speed ties; tied monsters keep list order.
// Dead monsters don't act.
pub fn turnOrder(p: Player, ms: list): list {
  return lists.sortBy([p] + livingMonsters(ms), fn(c) { return -c.speed })
}

// The chance to run from m: 50%, plus 10% for each point of speed the player has over it,
// kept between 10% and 90%.
pub fn fleeChance(p: Player, m: Monster): num ! {
  var chance = 0.5 + (p.speed - m.speed) * 0.1
  return math.clamp(chance, 0.1, 0.9)
}

// The player strikes m. Returns the damage done.
pub fn playerAttack(p: Player, m: Monster): num ! random {
  return applyDamage(m, rollDamage(attackPower(p), m.def))
}

// m strikes the player. Returns the damage done.
pub fn monsterAttack(m: Monster, p: Player): num ! random {
  return applyDamage(p, rollDamage(m.atk, defensePower(p)))
}

// A line for the battle log.
pub fn hitLine(attacker: str, target: str, dmg: num): str ! {
  if dmg == 0 { return "{attacker} misses {target}" }
  return "{attacker} hits {target} for {dmg}"
}

test "base damage and crits" {
  expect baseDamage(7, 3) == 4
  expect baseDamage(2, 9) == 1
  expect critMultiplier(0.05, 0.1) == 2
  expect critMultiplier(0.1, 0.1) == 1
}

test "rolled damage stays near base" {
  seed(7)
  for i in 0..50 {
    var d = rollDamage(8, 3)
    expect d >= 4 && d <= 11
  }
}

test "damage stops at zero hp" {
  var m = makeMonster("slime", 1)
  expect applyDamage(m, 5) == 5 && m.hp == 3
  expect applyDamage(m, 10) == 3 && m.hp == 0
  expect !isAlive(m)
}

test "battle ends when one side falls" {
  var p = Player("Ana")
  var ms = [makeMonster("slime", 1), makeMonster("wolf", 1)]
  expect !battleOver(p, ms)
  ms[0].hp = 0
  expect livingMonsters(ms) == [ms[1]]
  ms[1].hp = 0
  expect battleOver(p, ms)
}

test "turn order by speed, player first on ties" {
  var p = Player("Ana")
  var slime = makeMonster("slime", 1)
  var wolf = makeMonster("wolf", 1)
  var g = makeMonster("goblin", 1)
  g.speed = 5
  var order = turnOrder(p, [slime, g, wolf])
  expect order.len() == 4
  expect order[0] == wolf && order[1] == p && order[2] == g && order[3] == slime
  wolf.hp = 0
  expect turnOrder(p, [slime, g, wolf]).len() == 3
}

test "attacks lower hp" {
  seed(3)
  var p = Player("Ana")
  var m = makeMonster("skeleton", 1)
  var dealt = playerAttack(p, m)
  expect dealt >= 1 && m.hp == 18 - dealt
  var taken = monsterAttack(m, p)
  expect taken >= 1 && p.hp == 30 - taken
}

test "log lines" {
  expect hitLine("Ana", "wolf", 3) == "Ana hits wolf for 3"
  expect hitLine("wolf", "Ana", 0) == "wolf misses Ana"
}
