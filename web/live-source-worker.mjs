import { DurableObject } from "cloudflare:workers";
import { apply } from "./source-delta.mjs";
// Shared hot-reload service. The application chooses its binding names, entry point and placement.
export function liveSourceService(config) {
  const subscriberTag = config.subscriberTag ?? "subscriber";
  const publicRelay = env => env[config.namespace].getByName(config.name, config.locationHint ? { locationHint: config.locationHint } : {});
  class LiveSource extends DurableObject {
    constructor(ctx, env) {
      super(ctx, env);
      ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair("ping", "pong"));
      this.previous = config.previous && ctx.id.equals(env[config.namespace].idFromName(config.previous));
      this.sql = ctx.storage.sql;
      this.sql.exec("CREATE TABLE IF NOT EXISTS source(path TEXT PRIMARY KEY, text TEXT NOT NULL)");
      ctx.blockConcurrencyWhile(async () => {
        this.revision = ctx.storage.kv.get("revision");
        if (this.revision) {
          this.files = Object.fromEntries(this.sql.exec("SELECT path,text FROM source").raw());
        } else {
          if (this.previous || !config.previous) {
            const prior = config.seedBucket && await env[config.seedBucket].get(config.seedKey);
            this.files = prior ? await prior.json() : {};
          } else {
            // Seed once from the last committed public revision, then keep every
            // reconnect on the new relay. The old R2 seed can be much older.
            this.files = await env[config.namespace].getByName(config.previous).handoff();
          }
          ctx.storage.transactionSync(() => this.persist(this.files, this.files, crypto.randomUUID()));
        }
      });
    }

    persist(files, edits, revision) {
      for (const path of Object.keys(edits)) {
        if (path in files) this.sql.exec("INSERT OR REPLACE INTO source VALUES (?, ?)", path, files[path]);
        else this.sql.exec("DELETE FROM source WHERE path = ?", path);
      }
      this.ctx.storage.kv.put("revision", revision);
      this.revision = revision;
    }

    handoff() {
      for (const ws of this.ctx.getWebSockets()) ws.close(1012, "Relay moved");
      return this.files;
    }

    fetch(request) {
      if (this.previous) return publicRelay(this.env).fetch(request);
      const path = new URL(request.url).pathname;
      if (path === "/live/sources.json") {
        const headers = { ETag: '"' + this.revision + '"', "Cache-Control": "no-store" };
        if (request.headers.get("If-None-Match") === headers.ETag) return new Response(null, { status: 304, headers });
        if (!this.files[config.entry]) return new Response(null, { status: 404 });
        return Response.json(this.files, { headers });
      }
      if (request.headers.get("Upgrade")?.toLowerCase() !== "websocket") return new Response(null, { status: 426 });
      const [client, server] = Object.values(new WebSocketPair());
      this.ctx.acceptWebSocket(server, [path === "/live/publish" ? "publisher" : subscriberTag]);
      server.send(JSON.stringify({ files: this.files, revision: this.revision }));
      return new Response(null, { status: 101, webSocket: client });
    }

    webSocketMessage(ws, message) {
      if (this.previous) return ws.close(1012, "Relay moved");
      if (!this.ctx.getTags(ws).includes("publisher")) return ws.close(1008, "Read only");
      if (typeof message !== "string" || message.length > 1_000_000) return ws.close(1009, "Invalid update");
      const { base, patch, revision = crypto.randomUUID() } = JSON.parse(message);
      if (typeof revision !== "string" || revision === base || !/^[0-9a-f-]{36}$/.test(revision)) return ws.close(1008, "Invalid revision");
      if (base !== this.revision) return ws.close(1008, "Reconnect for current revision");
      const next = { ...this.files };
      apply(next, patch);
      if (!next[config.entry]) return ws.close(1008, "Missing entry point");
      // Forward the checked code immediately. Acknowledge the publisher only
      // after storage commits. A storage failure disconnects all sockets, forcing
      // both ends to resume from the last durable snapshot.
      const update = JSON.stringify({ base, patch, revision });
      for (const subscriber of this.ctx.getWebSockets(subscriberTag)) {
        try { subscriber.send(update); } catch { subscriber.close(); }
      }
      try {
        this.ctx.storage.transactionSync(() => this.persist(next, patch, revision));
        this.files = next;
      } catch {
        this.ctx.abort("Live source commit failed");
      }
      ws.send(JSON.stringify({ revision }));
    }

    webSocketClose(ws) { ws.close(); }
  }

  function fetch(request, env) {
    const url = new URL(request.url);
    const path = url.pathname;
    if (!["/live/sources.json", "/live/stream", "/live/publish"].includes(path)) return null;
    if (request.method !== "GET") return new Response(null, { status: 405, headers: { Allow: "GET" } });
    if (path === "/live/publish") {
      const auth = request.headers.get("Authorization") || "";
      const enc = new TextEncoder();
      const expected = enc.encode("Bearer " + env[config.secret]);
      const got = enc.encode(auth);
      if (!env[config.secret] || got.length !== expected.length || !crypto.subtle.timingSafeEqual(got, expected)) {
        return new Response(null, { status: 403 });
      }
    }
    return publicRelay(env).fetch(request);
  }
  return { Durable: LiveSource, fetch };
}
