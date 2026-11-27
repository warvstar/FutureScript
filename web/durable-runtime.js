// Cloudflare owns the request, storage and sockets. Fut receives opaque host handles and named events.
import { DurableObject } from "cloudflare:workers";
import { toResponse } from "./runtime.js";

class Host {
  constructor(ctx, env, deliver) { this.ctx = ctx; this.env = env; this.deliver = deliver; }
}

function requestEvent(request) {
  const url = new URL(request.url), query = Object.create(null);
  for (const [key, value] of url.searchParams) if (!Object.hasOwn(query, key)) query[key] = value;
  return { type: "fetch", request, method: request.method, url: request.url, origin: url.origin,
    pathname: url.pathname, query, headers: Object.fromEntries(request.headers) };
}
const response = value => value instanceof Response ? value : toResponse(value);

// An ordinary async entry serializes calls to its VM. A route that returns request actions runs
// only synchronous Fut turns. The originating fetch owns its own pending I/O and continuation.
export function workerHandler(openProgram, handler = "route", io = null) {
  let program;
  return async function fetch(request, env) {
    const fut = await (program ??= openProgram().catch(error => { program = null; throw error; }));
    const event = requestEvent(request), host = new Host(null, env, null);
    if (!io) return response(await fut.apply(handler, [event, host]));
    let action = fut.applySync(handler, [event, host]);
    while (action?.await) {
      let value = null, error = null;
      try {
        if (action.await === "body") value = await io.body(event, action.limit);
        else if (action.await === "rpc") value = await io.rpc(host, action.binding, action.name, action.method, action.arg);
        else throw new Error("Unknown request action: " + action.await);
      } catch (cause) { error = String(cause?.message ?? cause); }
      action = fut.applySync(handler, [{ ...event, type: "result", context: action.context, value, error }, host]);
    }
    if (action?.forward) {
      const { binding, name, url, headers } = action.forward;
      return io.forward(host, binding, name, url, headers);
    }
    return response(action);
  };
}

export function durableClass(openProgram, handler = "durable") {
  return class extends DurableObject {
    #host;
    #program;
    constructor(ctx, env) {
      super(ctx, env);
      this.#host = new Host(ctx, env, event => this.#event(event));
      this.#program = openProgram().then(async program => {
        await program.apply(handler, [{ type: "init" }, this.#host]);
        return program;
      });
      ctx.blockConcurrencyWhile(() => this.#program);
    }

    async #event(event) {
      const program = await this.#program;
      return program.apply(handler, [event, this.#host]);
    }

    async fetch(request) { return response(await this.#event(requestEvent(request))); }
    invoke(method, arg) { return this.#event({ type: "rpc", method, arg }); }
    alarm() { return this.#event({ type: "alarm" }); }

    webSocketMessage(socket, message) {
      return this.#event({ type: "message", socket,
        message: typeof message === "string" ? message : Array.from(new Uint8Array(message)) });
    }

    async webSocketClose(socket, code, reason, wasClean) {
      try { await this.#event({ type: "close", socket, code, reason, wasClean }); }
      finally {
        if (socket.readyState !== WebSocket.CLOSED) {
          if (code === 1000 || code >= 3000 && code <= 4999) socket.close(code, reason);
          else socket.close();
        }
      }
    }

    webSocketError(socket, error) {
      return this.#event({ type: "error", socket, message: String(error?.message ?? error) });
    }
  };
}
