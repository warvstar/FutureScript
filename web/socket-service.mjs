// Drive a Fut network program from either Node or the browser. The program chooses its
// endpoints, subscriptions and wire format. This host owns only fetch, sockets and clocks.
export class SocketService {
  constructor(openProgram, onStatus, origin, clock = performance) {
    this.onStatus = onStatus;
    this.origin = origin;
    this.clock = clock;
    this.sockets = new Map();
    this.connected = false;
    this.closed = false;
    this.canRetry = false;
    this.generation = 0;
    this.openProgram = openProgram;
    this.timer = setInterval(() => this.tick(), 100);
    this.retry();
  }

  invoke(op, fields = {}) {
    if (!this.program || this.closed) return null;
    const blocked = {};
    for (const [key, socket] of this.sockets) {
      if (socket.readyState !== WebSocket.OPEN || socket.bufferedAmount > 16384) blocked[key] = true;
    }
    const result = this.program({op, now:this.clock.now(), wall:Date.now(), blocked, ...fields});
    this.connected = result.connected;
    for (const action of result.actions ?? []) this.perform(action);
    return result;
  }

  perform(action) {
    const {op, key} = action;
    if (op === "login") {
      this.loginRequest = this.login(action);
    } else if (op === "status") {
      this.canRetry = action.retry;
      this.onStatus(action.text, action.retry);
    } else if (op === "connect") {
      this.connect(key, action);
    } else if (op === "close") {
      this.closeSocket(key);
    } else if (op === "send") {
      const socket = this.sockets.get(key);
      if (!socket) return;
      const data = action.data;
      try {
        socket.send(data?.bytes ? Uint8Array.from(data.bytes) : typeof data === "string" ? data : JSON.stringify(data));
      } catch { this.lost(key, socket); }
    }
  }

  async login({path, body}) {
    const generation = this.generation;
    try {
      const response = await fetch(new URL(path,this.origin), {
        method:"POST", headers:{"Content-Type":"application/json"}, body:JSON.stringify(body), signal:AbortSignal.timeout(10000),
      });
      if (!response.ok) throw Error("Session request failed");
      const value = await response.json();
      if (generation === this.generation) this.invoke("session", {value});
    } catch {
      if (generation === this.generation) this.invoke("unavailable");
    }
  }

  retry() {
    if (this.closed || this.loggingIn) return;
    this.generation++;
    this.loggingIn = (async () => {
      this.program ??= await this.openProgram();
      this.invoke("start");
      if (this.lastFrame) this.invoke("submit", {frame:this.lastFrame});
      await this.loginRequest;
    })().catch(error => {
      if (!this.closed) {
        this.canRetry = true;
        this.onStatus(error.message, true);
      }
    }).finally(() => { this.loggingIn = null; });
  }

  connect(key, {path, query}) {
    const url = new URL(path, this.origin);
    url.protocol = url.protocol === "https:" ? "wss:" : "ws:";
    for (const [name,value] of Object.entries(query ?? {})) url.searchParams.set(name,value);
    let socket;
    try { socket = new WebSocket(url); } catch { this.invoke("lost", {key}); return; }
    this.sockets.set(key,socket);
    socket.binaryType = "arraybuffer";
    socket.onmessage = ({data}) => {
      if (this.sockets.get(key) !== socket) return;
      try {
        if (data === "pong") this.invoke("pong", {key});
        else if (typeof data === "string") this.invoke("message", {key, message:JSON.parse(data)});
        else this.invoke("bytes", {key, bytes:Array.from(new Uint8Array(data))});
      } catch (error) {
        console.error("Socket input failed", error.message);
        this.lost(key,socket);
      }
    };
    socket.onclose = () => this.lost(key,socket);
  }

  lost(key,socket) {
    if (this.sockets.get(key) !== socket) return;
    this.invoke("lost", {key});
    if (this.sockets.get(key) === socket) this.closeSocket(key);
  }

  closeSocket(key) {
    const socket = this.sockets.get(key);
    this.sockets.delete(key);
    if (socket && socket.readyState < WebSocket.CLOSING) socket.close();
  }

  reset() { this.invoke("reset"); }
  present() { return this.connected; }
  tick() { this.invoke("tick"); }
  submit(frame) {
    if (!this.program) this.lastFrame = frame;
    else { this.lastFrame = null; this.invoke("submit", {frame}); }
  }
  snapshot() {
    const state = this.invoke("snapshot")?.snapshot;
    return state == null ? null : JSON.stringify(state);
  }
  stop() {
    this.closed = true;
    this.generation++;
    clearInterval(this.timer);
    for (const key of this.sockets.keys()) this.closeSocket(key);
    this.program = null;
  }
}
