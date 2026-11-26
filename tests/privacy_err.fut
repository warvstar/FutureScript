import "mods/shapes.fut" as shapes
print(shapes.unit())   // error: 'unit' is private to module 'shapes' (mark it pub there to use it here)
