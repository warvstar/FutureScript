// A small turn-based RPG: battles, rewards and the shop, on top of entities, combat and loot.

import "entities.fut"
import "combat.fut"
import "loot.fut"

// A fresh hero: a wooden sword in hand, a potion in the bag and 20 gold.
fn newHero(name: str): Player ! {
  var p = Player(name)
  var sword = Item("Wooden Sword", "weapon", 2, 3, 10)
  addItem(p, sword)
  equip(p, sword)
  addItem(p, potion())
  p.gold = 20
  return p
}

// A pack of monsters of one kind and level.
fn makePack(kind: str, level: num, count: num): list ! {
  var ms = []
  for i in 0..count { ms.push(makeMonster(kind, level)) }
  return ms
}

// Fights until one side is down or maxRounds have passed. Each round everyone standing acts in
// turnOrder: the player hits the first monster still standing, each monster hits the player.
// Returns {won, rounds, log}.
fn simulateBattle(p: Player, ms: list, maxRounds: num): map ! random {
  var log = []
  var rounds = 0
  while !battleOver(p, ms) && rounds < maxRounds {
    rounds += 1
    for c in turnOrder(p, ms) {
      if battleOver(p, ms) { break }
      if !isAlive(c) { continue }
      if isa(c, Player) {
        var target = livingMonsters(ms)[0]
        log.push(hitLine(p.name, target.name, playerAttack(p, target)))
      } else {
        log.push(hitLine(c.name, p.name, monsterAttack(c, p)))
      }
    }
  }
  return {won: isAlive(p) && livingMonsters(ms).len() == 0, rounds: rounds, log: log}
}

// The two log lines of one exchange: the player's hit, then the monster's answer.
fn narrateRound(p: Player, m: Monster, dealt: num, taken: num): list ! {
  return [hitLine(p.name, m.name, dealt), hitLine(m.name, p.name, taken)]
}

// One exchange with a single monster: the player strikes, and the monster answers if it still stands.
fn duelRound(p: Player, m: Monster): list ! random {
  var dealt = playerAttack(p, m)
  var taken = 0
  if isAlive(m) { taken = monsterAttack(m, p) }
  return narrateRound(p, m, dealt, taken)
}

test "a new hero is ready to go" {
  var p = newHero("Ana")
  expect p.weapon.name == "Wooden Sword"
  expect attackPower(p) == 7
  expect p.inventory.len() == 1 && p.inventory[0].kind == "potion"
  expect p.gold == 20
}

test "packs" {
  var ms = makePack("wolf", 2, 3)
  expect ms.len() == 3
  expect ms[0] != ms[1]
  expect ms[2].hp == 16 && ms[2].xpReward == 12
}

test "a strong hero beats a slime" {
  seed(5)
  var p = newHero("Ana")
  p.atk = 20
  var r = simulateBattle(p, [makeMonster("slime", 1)], 10)
  expect r.won && r.rounds == 1
  expect r.log.len() == 1 && r.log[0].has("Ana hits slime")
}

test "a battle can run out of rounds" {
  seed(9)
  var p = newHero("Ana")
  p.def = 50
  var tough = makeMonster("skeleton", 1)
  tough.def = 50
  tough.maxHp = 1000
  tough.hp = 1000
  var r = simulateBattle(p, [tough], 3)
  expect !r.won && r.rounds == 3 && r.log.len() == 6
  expect isAlive(p) && isAlive(tough)
}
