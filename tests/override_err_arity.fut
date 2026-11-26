class Shape { fn scale(k) { return k } }
class Rect : Shape { override fn scale(kx, ky) { return kx } }  // error: 'scale' takes 2 parameters, but Shape.scale, which it overrides, takes 1
