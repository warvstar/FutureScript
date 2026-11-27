// Values from the platform stay opaque in Fut. Only these operations use a Durable Object's capabilities.
export function sql(host, statement, params) {
  return host.ctx.storage.sql.exec(statement, ...(params ?? [])).toArray();
}
export function transaction(host, operation) { return host.ctx.storage.transactionSync(operation); }
export function get(host, key) { return host.ctx.storage.kv.get(key) ?? null; }
export function put(host, key, value) { host.ctx.storage.kv.put(key, value); }
export function remove(host, key) { return host.ctx.storage.kv.delete(key); }
export function upgrade(host, tags, attachment) {
  const [client, server] = Object.values(new WebSocketPair());
  host.ctx.acceptWebSocket(server, tags ?? []);
  if (attachment != null) server.serializeAttachment(attachment);
  return new Response(null, { status: 101, webSocket: client });
}
export function sockets(host, tag) { return host.ctx.getWebSockets(tag ?? undefined).filter(socket => socket.readyState === WebSocket.OPEN); }
export function attachment(socket) { return socket.deserializeAttachment(); }
export function attach(socket, value) { socket.serializeAttachment(value); }
export function send(socket, message) { socket.send(Array.isArray(message) ? Uint8Array.from(message) : message); }
export function close(socket, code, reason) {
  if (socket.readyState < WebSocket.CLOSING) socket.close(code, reason);
}
export function rpc(host, binding, name, method, arg) { return host.env[binding].getByName(name).invoke(method, arg); }
export function schedule(host, ms, tag) {
  setTimeout(() => host.deliver({ type: "timer", tag }), ms);
}
export function autoReply(host, request, response) {
  host.ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair(request, response));
}
export function uuid() { return crypto.randomUUID(); }
export function randomWords(n) { return Array.from(crypto.getRandomValues(new Uint32Array(n))); }
export function deferRpc(host, binding, name, method, arg, context) {
  const done = rpc(host, binding, name, method, arg).then(
    value => host.deliver({ type: "result", context, value, error: null }),
    error => host.deliver({ type: "result", context, value: null, error: String(error?.message ?? error) }),
  );
  host.ctx.waitUntil(done);
}
const utf8 = new TextEncoder(), decoder = new TextDecoder("utf-8", { fatal: true });
function pack(bytes) {
  return btoa(Array.from(bytes, b => String.fromCharCode(b)).join("")).replaceAll("+", "-").replaceAll("/", "_").replace(/=+$/, "");
}
function unpack(text) {
  if (!/^[A-Za-z0-9_-]*$/.test(text)) throw new TypeError("invalid base64url");
  return Uint8Array.from(atob(text.replaceAll("-", "+").replaceAll("_", "/")), c => c.charCodeAt(0));
}
export const encode64 = text => pack(utf8.encode(text));
export const decode64 = text => decoder.decode(unpack(text));
function key(host, name) {
  host.keys ??= new Map();
  if (!host.keys.has(name)) {
    const store = host.ctx.storage.kv;
    let raw = store.get(name);
    if (raw == null) { raw = crypto.getRandomValues(new Uint8Array(32)); store.put(name, raw); }
    host.keys.set(name, crypto.subtle.importKey("raw", raw, { name: "HMAC", hash: "SHA-256" }, false, ["sign", "verify"]));
  }
  return host.keys.get(name);
}
export async function hmacSign(host, keyName, text) {
  return pack(new Uint8Array(await crypto.subtle.sign("HMAC", await key(host, keyName), utf8.encode(text))));
}
export async function hmacVerify(host, keyName, text, signature) {
  const bytes = unpack(signature);
  return bytes.length === 32 && crypto.subtle.verify("HMAC", await key(host, keyName), bytes, utf8.encode(text));
}
export async function alarmAt(host, at) { await host.ctx.storage.setAlarm(at); }
export function forward(host, binding, name, url, headers) {
  return host.env[binding].getByName(name).fetch(new Request(url, { headers }));
}
export async function body(event, limit) {
  if (!Number.isSafeInteger(limit) || limit < 0) throw new RangeError("invalid body limit");
  const stream = event.request?.body;
  if (!stream) return null;
  const reader = stream.getReader(), chunks = [];
  let size = 0;
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      size += value.length;
      if (size > limit) { await reader.cancel(); return null; }
      chunks.push(value);
    }
  } finally { reader.releaseLock(); }
  const bytes = new Uint8Array(size);
  let offset = 0;
  for (const chunk of chunks) { bytes.set(chunk, offset); offset += chunk.length; }
  return decoder.decode(bytes);
}
export const matches = (text, pattern, flags) => new RegExp(pattern, flags).test(text);
export const uriComponent = text => encodeURIComponent(text);
export const decodeComponent = text => decodeURIComponent(text);

export const trim = text => text.trim();
