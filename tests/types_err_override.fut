class Shape { fn scale(k: num): num { return k } }
class Rect : Shape { override fn scale(k) { return k } }  // error: 'scale' must declare the same types as Shape.scale, which it overrides: (num): num, not (_)
