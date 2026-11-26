class Shape { fn area() { return 0 } }
class Rect : Shape { fn area() { return 6 } }  // error: 'area' overrides Shape.area: write 'override fn area' if that is intended
