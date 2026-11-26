// std/ui's DOM operations. In a browser they act on the page's document; elsewhere (fut test under Node) on a small
// document of their own, whose html() a test reads and whose fire() stands in for a user's click or keystroke.
const doc = globalThis.document ?? fakeDocument();

export const create = (tag) => doc.createElement(tag);
export const text = (s) => doc.createTextNode(s);
export const anchor = () => doc.createComment("");
export const fragment = () => doc.createDocumentFragment();
export const setText = (n, s) => void (n.data = s);
export const append = (p, c) => void p.appendChild(c);
export const put = (ref, n) => void ref.parentNode.insertBefore(n, ref);  // n (or a fragment's nodes) just before ref
export const remove = (n) => void n.parentNode?.removeChild(n);
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
    const r = f({ type, value: t.value ?? null, checked: !!t.checked, key: e.key ?? null });
    if (r && typeof r.catch === "function") r.catch(() => {});  // a fut error is reported by fut itself
  });
}
export const query = (sel) => (sel === "body" ? doc.body : doc.querySelector(sel));
export function css(s) { const e = doc.createElement("style"); e.textContent = s; (doc.head ?? doc.body).appendChild(e); return e; }
export function base(s) {  // a style sheet under the page's own: first in the head
  const e = doc.createElement("style"), h = doc.head ?? doc.body;
  e.textContent = s;
  h.insertBefore ? h.insertBefore(e, h.firstChild ?? null) : h.appendChild(e);
}
export const html = (n) => n.outerHTML ?? n.textContent;
export function fire(n, type, data) {  // as a user would: set what they typed or ticked, then the event
  if (data && "value" in data) n.value = data.value;
  if (data && "checked" in data) n.checked = data.checked;
  n.dispatchEvent(globalThis.document ? new KeyboardEvent(type, { bubbles: true, key: data?.key ?? "" }) : { type, target: n, key: data?.key });
}

// ---- data, timers, the address, storage, SVG ----
const fakes = new Map();  // url -> response, for tests and demos without a server
export const fake = (url, res) => void fakes.set(url, typeof res == "string" ? { status: 200, body: res, headers: {} } : res);
const later = (f, ...a) => { const p = f(...a); if (p && typeof p.catch == "function") p.catch(() => {}); };
export function request(req, f) {  // fetch, then f({status, body, headers}), or f({status: 0, error}): fut goes on meanwhile
  const url = typeof req == "string" ? req : req.url;
  if (fakes.has(url)) return void Promise.resolve().then(() => later(f, fakes.get(url)));
  const init = typeof req == "string" ? {} : { method: req.method ?? "GET", headers: req.headers ?? undefined, body: req.body ?? undefined };
  fetch(url, init).then(async (x) => later(f, { status: x.status, body: await x.text(), headers: Object.fromEntries(x.headers) }),
    (e) => later(f, { status: 0, error: String(e?.message ?? e) }));
}
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
export const createSvg = (tag) => doc.createElementNS ? doc.createElementNS("http://www.w3.org/2000/svg", tag) : doc.createElement(tag);

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
    removeChild(c) { const i = this.childNodes.indexOf(c); if (i >= 0) this.childNodes.splice(i, 1), (c.parentNode = null); return c; }
    get textContent() { return this.childNodes.map((c) => c.textContent).join(""); }
    set textContent(s) { for (const c of this.childNodes.slice()) this.removeChild(c); this.appendChild(new Text(s)); }
    get outerHTML() { return this.childNodes.map((c) => c.outerHTML).join(""); }
  }
  class Text extends Node { constructor(s) { super(); this.data = String(s); } get textContent() { return this.data; } get outerHTML() { return esc(this.data); } }
  class Comment extends Node { get textContent() { return ""; } get outerHTML() { return "<!---->"; } }
  class Fragment extends Node {}
  class Element extends Node {
    constructor(tag) { super(); this.tagName = tag; this.attrs = new Map(); this.listeners = {}; this.value = ""; this.checked = false; this.disabled = false; }
    setAttribute(k, v) { this.attrs.set(k, String(v)); }
    removeAttribute(k) { this.attrs.delete(k); }
    getAttribute(k) { return this.attrs.get(k) ?? null; }
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
