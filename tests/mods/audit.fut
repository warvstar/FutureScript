// An effect a module declares, and code that performs it.

// One line of an audit trail. Whoever runs the code decides where it goes.
pub effect Audit {
  fn record(line: str)
  fn count(): num
}

// Records a deposit; the caller handles Audit.
pub fn deposit(n: num): num ! Audit {
  Audit.record("deposit {n}")
  return Audit.count()
}
