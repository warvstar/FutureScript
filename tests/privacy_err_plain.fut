import "lib/limits.fut"
print(clamp(5, 0, 1))  // error: 'clamp' is private to limits.fut (mark it pub there to use it here)
