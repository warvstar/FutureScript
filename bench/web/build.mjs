// Bundles the JavaScript apps into dist/ for run.py: React and plain DOM with esbuild, Solid's JSX through its Babel preset.
import { build } from "esbuild";
import { transformAsync } from "@babel/core";
import { readFile } from "node:fs/promises";

const solid = {
  name: "solid",
  setup(b) {
    b.onLoad({ filter: /solid\.jsx$/ }, async (a) => ({
      contents: (await transformAsync(await readFile(a.path, "utf8"), { filename: a.path, presets: ["babel-preset-solid"] })).code,
      loader: "js",
    }));
  },
};
const common = { bundle: true, minify: true, format: "esm", outdir: "dist", define: { "process.env.NODE_ENV": '"production"' }, logLevel: "warning" };
await build({ ...common, entryPoints: ["apps/react.jsx", "apps/vanilla.js"], jsx: "automatic" });
await build({ ...common, entryPoints: ["apps/solid.jsx"], plugins: [solid], conditions: ["browser"] });
