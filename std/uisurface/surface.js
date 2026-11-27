// Hosts a std/ui view in a real modal. The game canvas has its own render resolution.
export const us_dom = () => 1;
const modals = new Map();
let next = 0;
export function us_open(title, rulesJSON, fontsJSON, dismiss, shortcut) {
  const id = ++next, root = document.createElement("dialog");
  const target = "fut-surface-" + id;
  root.id = target;
  root.setAttribute("aria-label", title);
  const host = document.createElement("div");
  host.id = target + "-view";
  root.append(host);
  const rules = JSON.parse(rulesJSON), style = document.createElement("style");
  const fonts = JSON.parse(fontsJSON).map(f =>
    `@font-face{font-family:"${f.name}";src:url("${f.path}");font-display:swap}`).join("\n");
  const css = Object.entries(rules).map(([name, props]) =>
    `#${target} .${CSS.escape(name)}{${Object.entries(props).map(([k,v])=>k+":"+v).join(";")}}`).join("\n");
  style.textContent = fonts + `
    #${target}{position:fixed;inset:0;width:100dvw;height:100dvh;overflow:hidden;max-width:none;max-height:none;margin:0;border:0;
      box-sizing:border-box;background:transparent;
      padding:env(safe-area-inset-top) env(safe-area-inset-right) env(safe-area-inset-bottom) env(safe-area-inset-left)}
    #${target}::backdrop{background:transparent}
    #${host.id}{width:100%;height:100%;overflow:hidden}
    #${target} *{box-sizing:border-box}
    #${host.id} *{display:flex;flex-direction:column;flex-shrink:0;margin:0;padding:0;border:0;
      min-width:0;font:inherit;color:inherit;background:transparent;letter-spacing:normal;border-radius:0}
    #${target} button{cursor:pointer;touch-action:manipulation}
    #${target} button:disabled{cursor:default}
    #${target} button:focus-visible{outline:2px solid #e6bb73;outline-offset:-3px}
    ` + css;
  document.head.append(style);
  const previous = document.activeElement;
  document.body.append(root);
  root.showModal();
  root.addEventListener("cancel", e => { e.preventDefault(); root.querySelector("#"+CSS.escape(dismiss))?.click(); });
  root.addEventListener("keydown", e => {
    if (shortcut && e.key.toLowerCase()===shortcut) {
      e.preventDefault();
      if (!e.repeat) root.querySelector("#"+CSS.escape(dismiss))?.click();
    }
  });
  let press, held = false;
  function cancelPress() {
    if (press) clearTimeout(press.timer);
    press = null;
  }
  root.addEventListener("pointerdown", e => {
    cancelPress();
    held = false;
    const button = e.target.closest("button[data-hold]");
    if (!button || e.pointerType === "mouse" || !e.isPrimary) return;
    press = {id:e.pointerId, x:e.clientX, y:e.clientY, timer:setTimeout(() => {
      if (button.isConnected) {
        held = true;
        button.dispatchEvent(new Event("hold"));
      }
      press = null;
    }, 550)};
  });
  root.addEventListener("pointermove", e => {
    if (press?.id === e.pointerId && Math.hypot(e.clientX-press.x,e.clientY-press.y)>8) cancelPress();
  });
  for (const event of ["pointercancel","pointerup"]) root.addEventListener(event, cancelPress);
  root.addEventListener("contextmenu", e => { if (e.target.closest("[data-hold]")) e.preventDefault(); });
  root.addEventListener("click", e => {
    if (held && e.detail) { e.preventDefault(); e.stopImmediatePropagation(); held = false; }
  }, true);
  const modal={root,host,style,previous,dismiss,cancelPress,point:null};
  root.addEventListener("pointermove", e => { modal.point=e.pointerType==="touch" ? null : [e.clientX,e.clientY]; });
  root.addEventListener("pointerleave", () => { modal.point=null; });
  root.addEventListener("keydown", e => { if (e.key==="Tab" || e.key.startsWith("Arrow")) modal.point=null; });
  modals.set(id,modal);
  return id;
}
export const us_target = id => "#" + modals.get(id).host.id;
export const us_width = id => modals.get(id).host.clientWidth;
export const us_height = id => modals.get(id).host.clientHeight;
export function us_active(id) {
  const m=modals.get(id), scroller=m.root.querySelector("[data-scroll-key]");
  m.scroll=scroller && {key:scroller.dataset.scrollKey,top:scroller.scrollTop};
  return m.root.contains(document.activeElement) ? document.activeElement.id : "";
}
export function us_focus(id, key) {
  const {root,dismiss,scroll} = modals.get(id);
  const scroller=root.querySelector("[data-scroll-key]");
  if (scroller && scroll?.key===scroller.dataset.scrollKey) scroller.scrollTop=scroll.top;
  const pick = key && root.querySelector("#"+CSS.escape(key));
  (pick?.matches("button:enabled") ? pick : root.querySelector("#"+CSS.escape(dismiss)))?.focus({preventScroll:true});
}
export function us_close(id) {
  const m=modals.get(id);
  if (!m) return;
  m.cancelPress();
  m.root.close(); m.root.remove(); m.style.remove(); modals.delete(id);
  if (m.previous?.isConnected) m.previous.focus({preventScroll:true});
}

// Logical viewport and visible scissor in the surface's coordinate system.
export function us_viewport(id, key) {
  const {host}=modals.get(id), el=host.querySelector("#"+CSS.escape(key));
  if (!el) return "null";
  const r=el.getBoundingClientRect(), origin=host.getBoundingClientRect();
  const clip=el.closest("[data-scroll-key]")?.getBoundingClientRect() ?? origin;
  const box={x:r.x-origin.x,y:r.y-origin.y,w:r.width,h:r.height,
    left:Math.max(r.left,clip.left)-origin.x,top:Math.max(r.top,clip.top)-origin.y,
    right:Math.min(r.right,clip.right)-origin.x,bottom:Math.min(r.bottom,clip.bottom)-origin.y};
  if (box.left>=box.right || box.top>=box.bottom) return "null";
  el.futViewport={...box, width:origin.width, height:origin.height};
  return JSON.stringify(box);
}

export function us_inspect(id) {
  const {root,point}=modals.get(id);
  const target=point ? document.elementFromPoint(...point) : document.activeElement;
  const item=target?.closest("[data-inspect]");
  return item && root.contains(item) ? item.id : "";
}
