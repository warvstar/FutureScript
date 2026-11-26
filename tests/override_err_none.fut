class Shape { fn area() { return 0 } }
class Rect : Shape { override fn perimeter() { return 6 } }  // error: 'perimeter' is marked override, but no superclass of Rect has a method 'perimeter'
