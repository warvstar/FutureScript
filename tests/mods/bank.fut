// Imported `as bank` by types.fut: a typed public interface.
pub class Account {
  var owner: str = ""
  var balance: num = 0
  var note: str? = nil
  fn init(owner: str) { self.owner = owner }
  // Takes n out; returns what is left.
  fn withdraw(n: num): num {
    if n > self.balance { throw "insufficient funds" }
    self.balance -= n
    return self.balance
  }
  fn deposit(n: num): num { self.balance += n return self.balance }
}
pub class Savings : Account {
  override fn withdraw(n: num): num { if n > 100 { throw "limit" } return super.withdraw(n) }
}
pub fn transfer(from: Account, to: Account, n: num) { from.withdraw(n) to.deposit(n) }
pub fn find(accounts: list, owner: str): Account? {
  for a in accounts { if a.owner == owner { return a } }
  return nil
}
