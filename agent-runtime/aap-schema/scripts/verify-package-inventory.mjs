import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import path from "node:path";

const packageRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const expectedFiles = [
  "README.md",
  "experimental/namespace.json",
  "fixtures/aap-core-domains.fixture-map.json",
  "fixtures/aap-core-domains.json",
  "fixtures/aap-core-generated-corpus.json",
  "fixtures/aap-initialize-compatible.jsonl",
  "fixtures/aap-initialize-incompatible.jsonl",
  "fixtures/aap-runtime-heartbeat.jsonl",
  "fixtures/aap-timeline-events.jsonl",
  "fixtures/aap-timeline-retention-gap.jsonl",
  "fixtures/aap-timeline-snapshot.jsonl",
  "fixtures/aap-timeline-sync.jsonl",
  "fixtures/aap-transport-definitions.fixture-map.json",
  "fixtures/aap-transport-methods.json",
  "fixtures/aap-transport-validation-corpus.json",
  "fixtures/aap-workspace-edit-proposal.jsonl",
  "fixtures/codex-provider-lifecycle-failures.jsonl",
  "fixtures/codex-recovery.jsonl",
  "fixtures/codex-thread-lifecycle.jsonl",
  "fixtures/codex-turn-metadata.jsonl",
  "fixtures/model-catalog-refresh-not-modified.json",
  "fixtures/turn-lifecycle.jsonl",
  "generated/cpp/aap_core_types_generated.cpp",
  "generated/cpp/aap_core_types_generated.h",
  "generated/cpp/aap_transport_types_generated.cpp",
  "generated/cpp/aap_transport_types_generated.h",
  "generated/typescript/core_types.d.ts",
  "generated/typescript/core_types.mjs",
  "generated/typescript/transport_types.d.ts",
  "generated/typescript/transport_types.mjs",
  "package.json",
  "runtime/cpp/aap_transport_runtime.cpp",
  "runtime/cpp/aap_transport_runtime.h",
  "scripts/emit-core-fixture.mjs",
  "scripts/generate-core-types.mjs",
  "scripts/generate-transport-types.mjs",
  "scripts/materialize-core-corpus.mjs",
  "scripts/materialize-transport-fixtures.mjs",
  "scripts/run-generated-transport-types.mjs",
  "scripts/run-transport-corpus.mjs",
  "scripts/test-core-generator-inputs.mjs",
  "scripts/test-transport-generator-inputs.mjs",
  "scripts/transport-schema-oracle.mjs",
  "scripts/verify-package-inventory.mjs",
  "stable/namespace.json",
  "stable/v0.1/aap.schema.json",
  "stable/v0.1/core.schema.json",
].sort();

const npmExecPath = process.env.npm_execpath;
const command = npmExecPath
  ? process.execPath
  : process.platform === "win32"
    ? "npm.cmd"
    : "npm";
const args = npmExecPath
  ? [npmExecPath, "pack", "--dry-run", "--json"]
  : ["pack", "--dry-run", "--json"];
const packed = spawnSync(command, args, {
  cwd: packageRoot,
  encoding: "utf8",
  maxBuffer: 4 * 1024 * 1024,
  // Modern Node refuses to spawn .cmd/.bat directly on Windows (EINVAL),
  // so the npm.cmd fallback must go through the shell.
  shell: process.platform === "win32" && !npmExecPath,
});
if (packed.error || packed.status !== 0) {
  throw new Error(`npm pack failed: ${packed.error?.message ?? packed.stderr.trim()}`);
}

let inventory;
try {
  inventory = JSON.parse(packed.stdout);
} catch {
  throw new Error("npm pack did not return JSON inventory");
}
if (!Array.isArray(inventory) || inventory.length !== 1
    || !Array.isArray(inventory[0]?.files)) {
  throw new Error("npm pack returned an unexpected inventory shape");
}

const actualFiles = inventory[0].files.map((entry) => entry?.path);
if (actualFiles.some((entry) => typeof entry !== "string")) {
  throw new Error("npm pack inventory contains an invalid path");
}
actualFiles.sort();
if (JSON.stringify(actualFiles) !== JSON.stringify(expectedFiles)) {
  const missing = expectedFiles.filter((entry) => !actualFiles.includes(entry));
  const unexpected = actualFiles.filter((entry) => !expectedFiles.includes(entry));
  throw new Error(
    `npm package inventory drift; missing=${missing.join(",") || "none"}; `
      + `unexpected=${unexpected.join(",") || "none"}`,
  );
}

process.stdout.write(`${actualFiles.length} package files verified\n`);
