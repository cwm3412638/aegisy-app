import { build } from "esbuild";
import { cp, mkdir, rm } from "node:fs/promises";
import path from "node:path";

const outdir = path.resolve(process.env.AEGISY_WEB_OUT_DIR || "dist");
await rm(outdir, { recursive: true, force: true });
await mkdir(outdir, { recursive: true });

await build({
  entryPoints: { editor: "src/editor.js", terminal: "src/terminal.js" },
  bundle: true,
  format: "iife",
  minify: true,
  sourcemap: false,
  outdir,
  assetNames: "assets/[name]-[hash]",
  loader: { ".ttf": "file" },
  legalComments: "linked"
});

const workers = {
  "editor.worker": "monaco-editor/esm/vs/editor/editor.worker.js",
  "json.worker": "monaco-editor/esm/vs/language/json/json.worker.js",
  "css.worker": "monaco-editor/esm/vs/language/css/css.worker.js",
  "html.worker": "monaco-editor/esm/vs/language/html/html.worker.js",
  "ts.worker": "monaco-editor/esm/vs/language/typescript/ts.worker.js"
};

await Promise.all(Object.entries(workers).map(([name, entry]) => build({
  entryPoints: [entry],
  bundle: true,
  format: "iife",
  minify: true,
  sourcemap: false,
  outfile: path.join(outdir, `${name}.js`),
  legalComments: "linked"
})));

await cp("src/index.html", path.join(outdir, "index.html"));
await cp("src/terminal.html", path.join(outdir, "terminal.html"));
await cp("node_modules/monaco-editor/LICENSE", path.join(outdir, "MONACO-LICENSE.txt"));
await cp("node_modules/@xterm/xterm/LICENSE", path.join(outdir, "XTERM-LICENSE.txt"));
await cp("node_modules/@xterm/addon-fit/LICENSE", path.join(outdir, "XTERM-ADDON-FIT-LICENSE.txt"));
