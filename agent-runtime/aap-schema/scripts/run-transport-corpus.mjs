import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { createTransportOracle, MAX_RAW_JSON_BYTES, parseTransportJson, stableDecisionIdentity } from "./transport-schema-oracle.mjs";

const PARSER_PROFILE = "exact-json-number-schema-bounded-integer-unicode-scalar-no-duplicate-keys/0.1";
const readMetadataJsonStrict = (path) => {
  const bytes = readFileSync(path);
  if (bytes.length > MAX_RAW_JSON_BYTES) throw new TypeError(`${path} exceeds the 4 MiB metadata limit`);
  const raw = new TextDecoder("utf-8", { fatal: true }).decode(bytes);
  parseTransportJson(raw);
  return JSON.parse(raw);
};

const exactKeys = (value, required, context) => {
  if (!value || typeof value !== "object" || Array.isArray(value)) throw new TypeError(`${context} must be an object`);
  const expected = new Set(required);
  for (const key of Object.keys(value)) if (!expected.has(key)) throw new TypeError(`${context} contains unknown field ${key}`);
  for (const key of required) if (!Object.hasOwn(value, key)) throw new TypeError(`${context} is missing ${key}`);
};

const [schemaArgument, corpusArgument] = process.argv.slice(2);
if (!schemaArgument || !corpusArgument || process.argv.length !== 4) {
  throw new Error("usage: run-transport-corpus.mjs <schema> <corpus>");
}
const schema = readMetadataJsonStrict(resolve(schemaArgument));
const oracle = createTransportOracle(schema);
const probeOracle = (name, probe) => createTransportOracle({
  $schema: "https://json-schema.org/draft/2020-12/schema",
  $id: `https://aegisy.cc/schemas/aap/oracle-probes/${name}.schema.json`,
  $defs: {},
  ...probe,
});
const probeOracles = new Map([
  ["$probe:anyOf", probeOracle("any-of", { anyOf: [{ type: "string", minLength: 1 }, { type: "integer", minimum: 1 }] })],
  ["$probe:constNumber", probeOracle("const-number", { const: 1 })],
  ["$probe:uniqueItems", probeOracle("unique-items", { type: "array", uniqueItems: true, items: true })],
]);
const corpus = readMetadataJsonStrict(resolve(corpusArgument));

exactKeys(corpus, ["schema_version", "schema_id", "parser_profile", "expected_decisions_sha256", "cases"], "corpus");
if (corpus.schema_version !== "aap-transport-validation-corpus/0.1" || corpus.schema_id !== schema.$id ||
    corpus.parser_profile !== PARSER_PROFILE) {
  throw new TypeError("corpus version, schema identity, or parser profile mismatch");
}
if (typeof corpus.expected_decisions_sha256 !== "string" || !/^[0-9a-f]{64}$/.test(corpus.expected_decisions_sha256)) {
  throw new TypeError("corpus decision identity must be a lowercase SHA-256 digest");
}
if (!Array.isArray(corpus.cases) || corpus.cases.length < 1 || corpus.cases.length > 128) {
  throw new TypeError("corpus cases must contain 1 through 128 entries");
}

const names = new Set();
const decisions = [];
for (const [index, entry] of corpus.cases.entries()) {
  exactKeys(entry, ["name", "target", "valid", "value_json"], `case ${index}`);
  if (typeof entry.name !== "string" || !/^[a-z0-9]+(?:-[a-z0-9]+)*$/.test(entry.name) || entry.name.length > 96 || names.has(entry.name)) {
    throw new TypeError(`case ${index} has an invalid or duplicate name`);
  }
  names.add(entry.name);
  if (entry.target !== "$root" && !probeOracles.has(entry.target) &&
      (typeof entry.target !== "string" || !oracle.definitionNames.includes(entry.target))) {
    throw new TypeError(`case ${entry.name} has an unknown target`);
  }
  if (typeof entry.valid !== "boolean" || typeof entry.value_json !== "string") {
    throw new TypeError(`case ${entry.name} has an invalid expectation or raw JSON`);
  }
  let accepted = true;
  try {
    if (entry.target === "$root") oracle.validateRootRaw(entry.value_json);
    else if (probeOracles.has(entry.target)) probeOracles.get(entry.target).validateRootRaw(entry.value_json);
    else oracle.validateDefinitionRaw(entry.target, entry.value_json);
  } catch {
    accepted = false;
  }
  if (accepted !== entry.valid) {
    throw new TypeError(`case ${entry.name} expected ${entry.valid ? "accept" : "reject"} but oracle ${accepted ? "accepted" : "rejected"} it`);
  }
  decisions.push({ name: entry.name, target: entry.target, accepted });
}

const digest = stableDecisionIdentity(`${corpus.schema_version}\t${corpus.parser_profile}`, decisions);
if (digest !== corpus.expected_decisions_sha256) {
  throw new TypeError(`corpus decision identity differs from golden: computed ${digest}`);
}
console.log(`${corpus.cases.length} ${digest}`);
