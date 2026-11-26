class Store { fn put(k) ! io { } }
class Cache : Store { override fn put(k) ! io, clock { } }  // error: 'put' must declare effects within those of Store.put, which it overrides (! io), not ! io, clock
