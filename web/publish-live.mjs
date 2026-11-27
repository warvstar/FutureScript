// Each publisher opens one connection and reconciles with the stream snapshot on reconnect.
import { watch } from "node:fs";
import { changes, isSource } from "./source-delta.mjs";
import { sourceCompiler } from "./source-compiler.mjs";

export async function publishLiveSources({ root, entry, token, url }) {
  let socket, revision, sent, latest;
  // Bound unacknowledged saves. If the link backs up, diff the newest good
  // snapshot against the last one sent as soon as a slot opens.
  const outstanding = [];
  let reconnectDelay = 250;

  function send() {
    if (!revision || outstanding.length >= 4 || !latest || socket.readyState !== WebSocket.OPEN) return;
    const patch = changes(sent, latest);
    if (!Object.keys(patch).length) return;
    const base = revision;
    revision = crypto.randomUUID();
    outstanding.push(revision);
    sent = latest;
    socket.send(JSON.stringify({ base, patch, revision }));
  }
  function connect() {
    socket = new WebSocket(url, { headers: { Authorization: "Bearer " + token } });
    let answered = true;
    const heartbeat = setInterval(() => {
      if (!answered) return socket.close();
      if (socket.readyState === WebSocket.OPEN) { answered = false; socket.send("ping"); }
    }, 30000);
    socket.onmessage = ({ data }) => {
      answered = true;
      if (data === "pong") return;
      const msg = JSON.parse(data);
      if (msg.files) {
        sent = msg.files;
        revision = msg.revision;
        outstanding.length = 0;
      } else if (outstanding.shift() !== msg.revision) {
        return socket.close();
      }
      reconnectDelay = 250;
      send();
    };
    socket.onclose = ({ code }) => {
      if (reconnectDelay === 250) console.error("Reconnecting live publisher, socket closed", code);
      clearInterval(heartbeat);
      revision = null;
      outstanding.length = 0;
      setTimeout(connect, reconnectDelay);
      reconnectDelay = Math.min(reconnectDelay * 2, 10000);
    };
  }

  const compile = await sourceCompiler(root, entry);
  let dirty = false, compiling = false;
  async function refresh() {
    dirty = true;
    if (compiling) return;
    compiling = true;
    try {
      while (dirty) {
        dirty = false;
        const { files } = await compile();
        latest = Object.fromEntries(Object.entries(files).filter(([p]) => isSource(p)));
        if (!dirty) send();
      }
    } catch (error) {
      console.error("Waiting for a valid save:", error.message);
    } finally {
      compiling = false;
      if (dirty) void refresh();
    }
  }
  let scheduled = false;
  watch(root, { recursive: true }, (_event, path) => {
    if (path && (!path.endsWith(".fut") || path.split("/").some(p => p.startsWith(".") || p === "dist" || p === "node_modules"))) return;
    if (scheduled) return;
    scheduled = true;
    setImmediate(() => { scheduled = false; void refresh(); });
  });
  connect();
  await refresh();
}
