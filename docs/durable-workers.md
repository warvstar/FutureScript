# Durable Workers

Build a hibernating WebSocket service from a Fut entry file:

```sh
fut worker build --durable Room --binding ROOMS examples/worker/room.fut build/room
cd build/room
npx wrangler dev
```

The generated Worker calls `route(request)` with the URL, method and headers. Return an instance name to forward the original request to that Durable Object, or return a regular Fut HTTP response. The generated Wrangler configuration creates a SQLite-backed namespace for the exported class.

The object receives `durable(event, host)` for each event:

| event.type | Fields | Result |
| --- | --- | --- |
| `init` | none | Ignored |
| `fetch` | method, url, origin, pathname, headers, query | An HTTP response or `cf.upgrade(host,tags,attachment)` |
| `message` | socket, message (text or list of bytes) | Ignored |
| `close` | socket, code, reason, wasClean | Ignored |
| `error` | socket, message | Ignored |
| `rpc` | method, arg | A value to return to the caller |
| `alarm`, `timer` | tag for timers | Ignored |
| `result` | context, value, error | Ignored |

Import `std/cloudflare` for SQL queries, synchronous KV storage, transactions, WebSockets and named-object RPC. `cf.sql(host,query,params)` returns a list of rows. Group related reads and writes in `cf.transaction(host,fn() { ... })`; its callback runs synchronously. `cf.rpc(host,"ROOMS",roomName,method,arg)` calls the other object's RPC handler and waits for its result. Scope each socket with tags, use `cf.sockets(host,tag)` for subscribers, and store per-socket state with `cf.attach` and `cf.attachment`. Socket handles and the host are opaque. After hibernation, Cloudflare preserves socket attachments and SQLite data; globals start fresh.

## Embedding a compiled Fut library

`fut worker build --library evaluate --root . src/policy.fut build/policy` builds a module exporting `openProgram()`. The build checks that the entry file defines `evaluate`. Each call to `openProgram()` creates an initialized, independent VM. Call a named top-level function through `program.applySync("evaluate",[argument])` for a synchronous decision, or `await program.apply("evaluate",[argument])` when Fut may wait for I/O. Synchronous calls can run inside a storage transaction and must finish before yielding. Choose a heap size with `--heap-mb`.

The generated `worker.js` imports precompiled Wasm modules for workerd. A browser or Node host can read the same artifacts with `readProgramBundle(baseURL, readBytes)` from the generated `runtime.js`. Cache the bundle, then call `openWorkerProgram(bundle)` for each independent stateful program.


## Serving concurrent requests

Compose multiple Durable classes and a Fut HTTP route with `writeWorkerEntry(bundleDirectory, descriptor)` from `web/worker-site.mjs`. Point Wrangler's main at `entry.js` in that private directory. The descriptor names the route `handler` and lists `objects` with their exported `className` and Fut `handler`. Each object has its own VM. Durable message, timer, alarm and RPC handlers on an object run serially. Use `cf.deferRpc(host,binding,name,method,arg,context)` to issue an RPC whose target may need to call back into this object. Its result arrives as a later event. `cf.schedule(host,ms,tag)` sets a short timer while the object is resident. Use `cf.alarmAt` for durable scheduling.

Set `requestActions: true` for an HTTP handler serving concurrent asynchronous requests. Each Fut invocation runs synchronously and returns an HTTP response or an operation. Return `{await:"body",limit:256,context:tag}` to read up to 256 bytes of UTF-8 text, or `{await:"rpc",binding,name,method,arg,context:tag}` to call another object. The Worker owns the I/O and calls the handler again with the original request fields plus `type:"result"`, `context`, `value` and `error`. Return `{forward:{binding,name,url,headers}}` to forward a GET, including a WebSocket upgrade. In a serial handler, `cf.body(event,limit)` reads bounded UTF-8 text directly.

For live source distribution, an optional `live` descriptor names a relay class, namespace, secret binding, singleton name and entry file. Its public stream sends an initial snapshot and then versioned text edits. Use `publishLiveSources({root,entry,url,token})` from `web/publish-live.mjs` for the local publisher. The relay stores committed files and revision in SQLite-backed Durable Object storage. Restrict the publisher secret to machines that may change the public program.
