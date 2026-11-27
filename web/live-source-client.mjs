import { apply, changes } from "./source-delta.mjs";

export async function followLiveSources(files, onChange, entry, streamPath = "/live/stream") {
  let first;
  const ready = new Promise(resolve => { first = resolve; });
  let delay = 250;
  function connect() {
    const url = new URL(streamPath, location.href);
    url.protocol = url.protocol === "https:" ? "wss:" : "ws:";
    const socket = new WebSocket(url);
    let revision;
    socket.onmessage = ({ data }) => {
      try {
        const msg = JSON.parse(data);
        if (!msg.files && (!revision || msg.base !== revision)) return socket.close();
        const patch = msg.files ? changes(files, msg.files) : msg.patch;
        if (Object.keys(patch).length && (!msg.files || msg.files[entry])) {
          apply(files, patch);
          onChange();
        }
        revision = msg.revision;
        delay = 250;
        first();
      } catch { socket.close(); }
    };
    socket.onclose = () => {
      setTimeout(connect, delay);
      delay = Math.min(delay * 2, 10000);
    };
  }
  connect();
  // Start from the deployed sources if the relay is unreachable.
  await Promise.race([ready, new Promise(resolve => setTimeout(resolve, 1500))]);
}
