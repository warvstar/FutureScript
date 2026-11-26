// yield directly inside atomic { } is a compile error
var f = fiber(fn() {
  atomic {
    yield 1
  }
})
// error: 'yield' inside atomic { }: other code would run while it is half done (yield after it ends)
