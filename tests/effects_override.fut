// An override may declare fewer effects than the method it replaces (callers holding the base still get what they rely on).
class Store {
  fn put(k) ! io, clock { print("store {k}") }
  fn get(k) { return k }
}
class Memory : Store {
  override fn put(k) ! { }
  override fn get(k) ! { return k * 2 }   // the base is unchecked: an override may declare anything
}
Store().put(1)  // expect: store 1
Memory().put(2)
print(Memory().get(3))  // expect: 6
