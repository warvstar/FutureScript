extern mouse fn CGEventSourceButtonState(int,int): int

// 0 is primary, 1 secondary, and 2 middle.
pub fn down(button: num): bool {
  return button==floor(button) && button>=0 && button<=4 && CGEventSourceButtonState(0,button)!=0
}
