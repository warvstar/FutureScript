// A pub function's contract is part of its .api, so it may use only pub names: `total` is private here.
fn total(l) { var s = 0 for x in l { s += x } return s }
pub fn spread(l: list): num
  ensures total(l) >= 0
{
  return 0
}
print(spread([1]))
