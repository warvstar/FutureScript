// std/ui's DOM operations. In a browser they act on the page's document; elsewhere (fut test under Node) on a small
// document of their own, whose html() a test reads and whose fire() stands in for a user's click or keystroke.
const doc = globalThis.document ?? fakeDocument();

// fresh: it makes a new object, so a fut thread calling it from a worker needn't wait for it (see fut.js's flush)
const fresh = (f) => ((f.fresh = true), f);
const freshList = (f) => ((f.freshList = true), f);  // a list of new objects: as many as its last argument says
export const create = fresh((tag) => doc.createElement(tag));
export const text = fresh((s) => doc.createTextNode(s));
export const anchor = fresh(() => doc.createComment(""));
export const fragment = fresh(() => doc.createDocumentFragment());
export const setText = (n, s) => void (n.data = s);
export const append = (p, c) => void p.appendChild(c);
export const put = (ref, n) => void ref.parentNode.insertBefore(n, ref);  // n (or a fragment's nodes) just before ref
export const remove = (n) => void n.parentNode?.removeChild(n);
// ---- templates: a view's fixed part, made once and cloned for each use (src/views.cpp, ui.tpl) ----
// desc: text, null (a place for a child: a comment), or [tag, attrs, flags, ...children]; flags 1: fut wires the
// element (its live props, handlers), 2: its one child is text fut keeps current (an empty text node, given).
const templates = [];
export function template(desc) {
  const paths = [];  // where the nodes fut wires are, in order
  const make = (d, path) => {
    if (d === null) return paths.push(path), doc.createComment("");
    if (!Array.isArray(d)) return doc.createTextNode(String(d));
    const [tag, attrs, flags, ...kids] = d, e = doc.createElement(tag);
    for (const k in attrs) if (attrs[k] != null && attrs[k] !== false) e.setAttribute(k, attrs[k] === true ? "" : String(attrs[k]));
    if (flags & 1 && path.length) paths.push(path);  // (the root is the first node anyway)
    if (flags & 2) paths.push([...path, 0]), e.appendChild(doc.createTextNode(""));
    kids.forEach((k, i) => e.appendChild(make(k, [...path, i])));
    return e;
  };
  return templates.push({ root: make(desc, []), paths }) - 1;
}
// A copy of template id, then the nodes fut wires: [root, ...] (count of them: a worker gives each an id first)
export const instance = freshList((id, count) => {
  const { root, paths } = templates[id], n = root.cloneNode(true), out = [n];
  for (const p of paths) { let x = n; for (const i of p) x = x.childNodes[i]; out.push(x); }
  return out;
});
export function clear(n) { while (n.firstChild) n.removeChild(n.firstChild); }
export function clearBetween(a, b) { while (a.nextSibling && a.nextSibling !== b) a.parentNode.removeChild(a.nextSibling); }
export function moveRange(a, b, ref) {  // the nodes from a to b, inclusive, to just before ref
  const p = ref.parentNode;
  for (let n = a, next; n; n = next) { next = n === b ? null : n.nextSibling; p.insertBefore(n, ref); }
}
export function removeRange(a, b) {
  for (let n = a, next; n; n = next) { next = n === b ? null : n.nextSibling; n.parentNode.removeChild(n); }
}
export function setProp(n, k, v) {
  if (k === "value") {
    n.value = v == null ? "" : String(v);
    if (n.tagName === "SELECT" && typeof queueMicrotask === "function") queueMicrotask(() => (n.value = v == null ? "" : String(v)));  // its options may come after
  }
  else if (k === "checked" || k === "disabled") n[k] = !!v;
  else if (v == null || v === false) n.removeAttribute(k);
  else n.setAttribute(k, v === true ? "" : String(v));
}
export function listen(n, type, f) {
  n.addEventListener(type, (e) => {
    const t = e.target ?? {};
    if (type === "submit") e.preventDefault?.();  // a form's submit is the page's to handle: no navigation
    const at = e.clientX == null ? null : n.getBoundingClientRect?.() ?? { left: 0, top: 0 };  // pointer events: where
    const r = f({ type, value: t.value ?? null, checked: !!t.checked, key: e.key ?? null, direct: t === n,
                  top: n.scrollTop ?? 0, height: n.clientHeight ?? 0,
                  x: at && e.clientX - at.left, y: at && e.clientY - at.top,  // from the handler's element's corner
                  clientX: e.clientX ?? null, clientY: e.clientY ?? null, button: e.button ?? null,  // in the window
                  ctrl: !!e.ctrlKey, shift: !!e.shiftKey, alt: !!e.altKey, meta: !!e.metaKey });
    if (r && typeof r.catch === "function") r.catch(() => {});  // a fut error is reported by fut itself
  });
}
export const query = (sel) => (sel === "body" ? doc.body : sel === "head" ? doc.head : doc.querySelector(sel));
export const css = fresh((s) => { const e = doc.createElement("style"); e.textContent = s; (doc.head ?? doc.body).appendChild(e); return e; });
export function base(s) {  // a style sheet under the page's own: first in the head
  for (const p of doc.querySelectorAll?.("style[data-prerendered]") ?? []) p.remove();  // a static site's, from its build: the page's own sheets are in now
  const e = doc.createElement("style"), h = doc.head ?? doc.body;
  e.textContent = s;
  h.insertBefore ? h.insertBefore(e, h.firstChild ?? null) : h.appendChild(e);
}
export const html = (n) => n.outerHTML ?? n.textContent;
export function fire(n, type, data) {  // as a user would: set what they typed or ticked, then the event
  if (data && "value" in data) n.value = data.value;
  if (data && "checked" in data) n.checked = data.checked;
  if (data && "top" in data) n.scrollTop = data.top;
  if (data && "height" in data && !globalThis.document) n.clientHeight = data.height;
  const mods = { ctrlKey: !!data?.ctrl, shiftKey: !!data?.shift, altKey: !!data?.alt, metaKey: !!data?.meta };
  const at = data && "x" in data ? { clientX: data.x, clientY: data.y ?? 0, button: data.button ?? 0 } : {};  // x, y: in the window
  n.dispatchEvent(!globalThis.document ? { type, target: n, key: data?.key, files: data?.files, ...mods, ...at }
    : /^(pointer|mouse|click|dblclick|contextmenu)/.test(type) ? new MouseEvent(type, { bubbles: true, ...mods, ...at })
    : new KeyboardEvent(type, { bubbles: true, key: data?.key ?? "", ...mods }));
}

// ---- data, timers, the address, storage, SVG ----

const fakes = new Map();  // url -> response, for tests and demos without a server
export const fake = (url, res) => void fakes.set(url, typeof res == "string" ? { status: 200, body: res, headers: {} } : res);
const later = (f, ...a) => { const p = f(...a); if (p && typeof p.catch == "function") p.catch(() => {}); };
// A static site's build shows the page here, as it first is (web/site.mjs): what it would fetch stays loading.
let prerendering = false;
export const prerender = () => void (prerendering = true);
export function request(req, f) {  // fetch, then f({status, body, headers}), or f({status: 0, error}): fut goes on meanwhile
  if (prerendering) return;
  const url = typeof req == "string" ? req : req.url;
  if (fakes.has(url)) return void Promise.resolve().then(() => later(f, fakes.get(url)));
  const init = typeof req == "string" ? {} : { method: req.method ?? "GET", headers: req.headers ?? undefined, body: req.body ?? undefined };
  fetch(url, init).then(async (x) => later(f, { status: x.status, body: await x.text(), headers: Object.fromEntries(x.headers) }),
    (e) => later(f, { status: 0, error: String(e?.message ?? e) }));
}
// ---- files ----
// A file as fut sees it: {name, size, type, file} (file: the File itself, held by reference).
const fileOf = (f) => ({ name: f.name, size: f.size ?? 0, type: f.type ?? "", file: f });
// A zone (a label holding an <input type=file>) whose files f gets: chosen, or dropped on it (class "over" while a drag is
// over it). Under Node, trigger(zone, "drop", {files}) drops plain {name, size, type, text} maps.
export function filesFrom(zone, input, f) {
  const give = (list) => list && list.length && later(f, [...list].map(fileOf));
  input.addEventListener("change", () => { give(input.files); if (globalThis.document) input.value = ""; });
  let depth = 0;  // dragenter and dragleave come for each child crossed
  const over = (on) => zone.classList ? zone.classList.toggle("over", on) : on ? zone.setAttribute("data-over", "") : zone.removeAttribute("data-over");
  zone.addEventListener("dragenter", (e) => { e.preventDefault?.(); depth++; over(true); });
  zone.addEventListener("dragover", (e) => e.preventDefault?.());  // (else the browser opens the file)
  zone.addEventListener("dragleave", () => { if (--depth <= 0) depth = 0, over(false); });
  zone.addEventListener("drop", (e) => {
    e.preventDefault?.();
    depth = 0, over(false);
    let list = e.dataTransfer?.files ?? e.files;
    if (list && !(input.multiple ?? input.hasAttribute("multiple"))) list = [...list].slice(0, 1);
    give(list);
  });
}
// A file's contents: as "text", or "url" (a data: URL, for an <img>), then f({value}) or f({error}).
export function readFile(file, as, f) {
  const fl = file?.file ?? file;
  if (typeof FileReader == "undefined" || !(fl instanceof Blob))  // Node: a dropped map's text
    return void Promise.resolve().then(() => later(f, fl?.text != null ? { value: as == "url" ? `data:${fl.type};base64,` + btoa(fl.text) : fl.text } : { error: "no file to read" }));
  const r = new FileReader();
  r.onload = () => later(f, { value: r.result });
  r.onerror = () => later(f, { error: String(r.error?.message ?? r.error) });
  as == "url" ? r.readAsDataURL(fl) : r.readAsText(fl);
}
// An address that shows a file (an image's preview), until the page goes: blob:...
export const fileUrl = (file) => { const fl = file?.file ?? file; return typeof URL.createObjectURL == "function" && fl instanceof Blob ? URL.createObjectURL(fl) : ""; };
// Send a file (or {name: file|text}, as a form) to req's url: progress(sent, total) as it goes, then done({status, body})
// or done({status: 0, error}). A url respond() faked answers at once. Returns what stopUpload() takes.
export function sendFile(req, body, progress, done) {
  const url = typeof req == "string" ? req : req.url;
  if (fakes.has(url)) return void Promise.resolve().then(() => { later(progress, 1, 1); later(done, fakes.get(url)); }), null;
  const x = new XMLHttpRequest();
  x.open(req.method ?? "POST", url);
  for (const [k, v] of Object.entries(req.headers ?? {})) x.setRequestHeader(k, v);
  x.upload.onprogress = (e) => later(progress, e.loaded, e.lengthComputable ? e.total : 0);
  x.onload = () => later(done, { status: x.status, body: x.responseText });
  x.onerror = () => later(done, { status: 0, error: "the upload failed: no answer from " + url });
  x.onabort = () => later(done, { status: 0, error: "stopped" });
  let data = body?.file ?? body;
  if (data && typeof data == "object" && !(data instanceof Blob)) {
    const form = new FormData();
    for (const [k, v] of Object.entries(data)) {
      const x = v?.file ?? v;
      x instanceof Blob ? form.append(k, x, v?.name ?? x.name) : form.append(k, typeof x == "string" ? x : JSON.stringify(x));
    }
    data = form;
  }
  x.send(data);
  return x;
}
export const stopUpload = (x) => void x?.abort?.();

export function timer(ms, repeat, f) {  // an id for stopTimer; under Node it doesn't keep the process alive
  const t = (repeat ? setInterval : setTimeout)(() => later(f), ms);
  t?.unref?.();
  return t;
}
export const stopTimer = (t) => void (clearInterval(t), clearTimeout(t));
let fakeHash = "";
const hashFns = [];
export const hash = () => decodeURIComponent(globalThis.location ? location.hash.slice(1) : fakeHash);
export function setHash(h) {
  if (globalThis.location) location.hash = h;
  else if (h != fakeHash) { fakeHash = h; for (const f of hashFns) later(f); }
}
export function onHash(f) { globalThis.location ? addEventListener("hashchange", () => later(f)) : hashFns.push(f); }
const mem = new Map();
export function stored(k) { try { return globalThis.document && globalThis.localStorage ? localStorage.getItem(k) : mem.get(k) ?? null; } catch { return null; } }
export function store(k, v) { try { globalThis.document && globalThis.localStorage ? localStorage.setItem(k, v) : mem.set(k, v); } catch {} }
export function modal(n, on) {  // a <dialog> shown modally, or closed (once it is on the page)
  if (!n.showModal) return void (on ? n.setAttribute("open", "") : n.removeAttribute("open"));
  if (on && !n.isConnected) return void queueMicrotask(() => modal(n, on));
  if (on && !n.open) n.showModal();
  else if (!on && n.open) n.close();
}
// The dev page's state inspector (web/index.html defines __futState): each named state's {value}, or {gone: true}.
export const inspecting = () => (typeof globalThis.__futState == "function" ? 1 : 0);
export const inspect = (name, v) => void globalThis.__futState?.(name, v);
export const focus = (n) => void n.focus?.();
// A menu's button b and its list: the list is a popover b toggles (light dismiss: a click outside or Escape closes it),
// placed under b; choosing an item closes it; the arrow keys move between its items. Without popovers (or a page), the
// list is hidden until b is clicked.
export function dropdown(b, list) {
  const items = () => [...(list.querySelectorAll?.("button, a[href], [role=menuitem]") ?? [])];
  const pop = typeof list.showPopover === "function";
  const open = () => (pop ? list.matches(":popover-open") : !list.hasAttribute("hidden"));
  const show = (on) => {
    if (pop) return void (on ? list.showPopover() : list.hidePopover());
    on ? list.removeAttribute("hidden") : list.setAttribute("hidden", "");
    b.setAttribute("aria-expanded", String(on));
  };
  if (pop) {
    list.setAttribute("popover", "auto");
    b.popoverTargetElement = list;  // b toggles it, and a click on b isn't one outside
    list.addEventListener("beforetoggle", (e) => {
      if (e.newState !== "open") return;
      const r = b.getBoundingClientRect();
      list.style.top = `${r.bottom + 4}px`;
      list.style.left = `${Math.min(r.left, innerWidth - list.offsetWidth - 8)}px`;
    });
    list.addEventListener("toggle", (e) => {
      b.setAttribute("aria-expanded", String(e.newState === "open"));
      if (e.newState === "open") for (const it of items()) it.setAttribute("role", "menuitem");
      if (e.newState === "open") items()[0]?.focus();
    });
  } else {
    list.setAttribute("hidden", "");
    b.addEventListener("click", () => show(!open()));
  }
  list.addEventListener("click", (e) => { if (e.target !== list) show(false); });
  list.addEventListener("keydown", (e) => {
    const all = items(), at = all.indexOf(doc.activeElement);
    const to = { ArrowDown: at + 1, ArrowUp: at - 1, Home: 0, End: all.length - 1 }[e.key];
    if (to === undefined || !all.length) return;
    e.preventDefault?.();
    all[(to + all.length) % all.length].focus();
  });
}
export const measure = (n) => ({ top: n.scrollTop ?? 0, height: n.clientHeight ?? 0 });
export const createSvg = fresh((tag) => doc.createElementNS ? doc.createElementNS("http://www.w3.org/2000/svg", tag) : doc.createElement(tag));

// ---- a document for Node: elements, text, comments and fragments; attributes; listeners; outerHTML ----
function fakeDocument() {
  const esc = (s) => String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
  class Node {
    constructor() { this.parentNode = null; this.childNodes = []; }
    get firstChild() { return this.childNodes[0] ?? null; }
    get nextSibling() { const s = this.parentNode?.childNodes; return s ? s[s.indexOf(this) + 1] ?? null : null; }
    appendChild(c) { return this.insertBefore(c, null); }
    insertBefore(c, ref) {
      const kids = c instanceof Fragment ? c.childNodes.slice() : [c];
      for (const k of kids) {
        k.parentNode?.removeChild(k);
        const i = ref ? this.childNodes.indexOf(ref) : -1;
        i < 0 ? this.childNodes.push(k) : this.childNodes.splice(i, 0, k);
        k.parentNode = this;
      }
      return c;
    }
    cloneNode(deep) { const c = this.shallow(); if (deep) for (const k of this.childNodes) c.appendChild(k.cloneNode(true)); return c; }
    removeChild(c) { const i = this.childNodes.indexOf(c); if (i >= 0) this.childNodes.splice(i, 1), (c.parentNode = null); return c; }
    get textContent() { return this.childNodes.map((c) => c.textContent).join(""); }
    set textContent(s) { for (const c of this.childNodes.slice()) this.removeChild(c); this.appendChild(new Text(s)); }
    get outerHTML() { return this.childNodes.map((c) => c.outerHTML).join(""); }
  }
  class Text extends Node { constructor(s) { super(); this.data = String(s); } get textContent() { return this.data; } get outerHTML() { return esc(this.data); } shallow() { return new Text(this.data); } }
  class Comment extends Node { get textContent() { return ""; } get outerHTML() { return "<!---->"; } shallow() { return new Comment(); } }
  class Fragment extends Node { shallow() { return new Fragment(); } }
  class Element extends Node {
    constructor(tag) { super(); this.tagName = tag; this.attrs = new Map(); this.listeners = {}; this.value = ""; this.checked = false; this.disabled = false; }
    setAttribute(k, v) { this.attrs.set(k, String(v)); }
    removeAttribute(k) { this.attrs.delete(k); }
    getAttribute(k) { return this.attrs.get(k) ?? null; }
    hasAttribute(k) { return this.attrs.has(k); }
    shallow() { const e = new Element(this.tagName); e.attrs = new Map(this.attrs); return e; }
    addEventListener(t, f) { (this.listeners[t] ??= []).push(f); }
    dispatchEvent(e) { e.target ??= this; for (let n = this; n; n = n.parentNode) for (const f of n.listeners?.[e.type] ?? []) f(e); }
    get outerHTML() {
      let a = "";
      for (const [k, v] of this.attrs) a += v === "" ? ` ${k}` : ` ${k}="${esc(v)}"`;
      if (this.tagName === "input") a += (this.value ? ` value="${esc(this.value)}"` : "") + (this.checked ? " checked" : "");
      if (this.disabled) a += " disabled";
      return this.tagName === "input" ? `<input${a}>` : `<${this.tagName}${a}>${super.outerHTML}</${this.tagName}>`;
    }
  }
  const d = {
    createElement: (t) => new Element(t), createTextNode: (s) => new Text(s), createComment: () => new Comment(),
    createDocumentFragment: () => new Fragment(), querySelector: (s) => (s.startsWith("#") ? find(d.body, s.slice(1)) : null),
    body: new Element("body"), head: new Element("head"),
  };
  const find = (n, id) => n.getAttribute?.("id") === id ? n : n.childNodes.reduce((r, c) => r ?? find(c, id), null);
  return d;
}
