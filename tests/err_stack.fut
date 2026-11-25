fn down(n) { return down(n + 1) + 1 }
down(0)
// error: stack overflow
