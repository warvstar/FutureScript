// One thread of fut-mt.wasm (the threads build), on a Node worker: its own instance of fut.wasm over the shared
// memory, running wasi_thread_start (a pool thread's loop, or the ticker) until the process ends.
import { workerData } from "node:worker_threads";
import { createFut } from "./fut.js";
import { nodeOptions } from "./node.mjs";

const { cfg, thread } = workerData;
await createFut({ ...nodeOptions(cfg), wasm: thread.module, thread });
