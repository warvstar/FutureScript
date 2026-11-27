let buttons = 0;
if (typeof document !== "undefined") {
  const update = event => { if (event.pointerType !== "touch") buttons = event.buttons; };
  for (const type of ["pointerdown","pointermove","pointerup","pointercancel"])
    addEventListener(type, update, {capture:true, passive:true});
  addEventListener("blur", () => { buttons = 0; });
}

export function CGEventSourceButtonState(_source, button) {
  return Number((buttons & (1 << button)) !== 0);
}
