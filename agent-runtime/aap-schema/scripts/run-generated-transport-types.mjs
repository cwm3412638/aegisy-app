import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import {
  canonicalTransportJson,
  isTransportJsonNumber,
  TRANSPORT_SCHEMA_ID,
  validateTransportDefinitionRaw,
  validateTransportMessageRaw,
} from "../generated/typescript/transport_types.mjs";
import {
  createTransportOracle,
  MAX_RAW_JSON_BYTES,
  parseTransportJson,
  stableDecisionIdentity,
} from "./transport-schema-oracle.mjs";

const CORPUS_SCHEMA = "aap-transport-validation-corpus/0.1";
const PARSER_PROFILE = "exact-json-number-schema-bounded-integer-unicode-scalar-no-duplicate-keys/0.1";

const readMetadata = (path) => {
  const bytes = readFileSync(resolve(path));
  if (bytes.length > MAX_RAW_JSON_BYTES) throw new TypeError("metadata exceeds the 4 MiB limit");
  const raw = new TextDecoder("utf-8", { fatal: true }).decode(bytes);
  parseTransportJson(raw);
  return JSON.parse(raw);
};

const exactKeys = (value, keys, context) => {
  if (!value || typeof value !== "object" || Array.isArray(value) ||
      Object.keys(value).length !== keys.length ||
      Object.keys(value).some((key) => !keys.includes(key)) ||
      keys.some((key) => !Object.hasOwn(value, key))) {
    throw new TypeError(`${context} fields are invalid`);
  }
};

const verifyPublicNumberContract = () => {
  const error = validateTransportDefinitionRaw(
    "jsonRpcError",
    '{"code":-100.000e-2,"message":"error"}',
  );
  const number = error.code;
  if (!isTransportJsonNumber(number) || number.lexical !== "-100.000e-2" ||
      number.canonical !== "-1" || number.integer !== true) {
    throw new TypeError("generated TransportJsonNumber fields differ from the runtime value");
  }
  if (canonicalTransportJson(number) !== "-1") {
    throw new TypeError("generated TransportJsonNumber cannot be canonicalized through its public value");
  }

  const lookalike = Object.freeze({ lexical: "1", canonical: "1", integer: true });
  if (isTransportJsonNumber(lookalike) ||
      canonicalTransportJson(lookalike) !== '{"canonical":"1","integer":true,"lexical":"1"}') {
    throw new TypeError("an ordinary object was mistaken for a TransportJsonNumber");
  }

  const copied = Object.create(
    Object.getPrototypeOf(number),
    Object.getOwnPropertyDescriptors(number),
  );
  if (isTransportJsonNumber(copied)) {
    throw new TypeError("copied runtime properties forged a TransportJsonNumber");
  }
  let copiedCanonical = null;
  try {
    copiedCanonical = canonicalTransportJson(copied);
  } catch (error_) {
    if (!(error_ instanceof TypeError)) throw error_;
  }
  if (copiedCanonical === "-1") {
    throw new TypeError("copied runtime properties were canonicalized as a TransportJsonNumber");
  }
};

verifyPublicNumberContract();

const emitFixtures = (path) => {
  const catalog = readMetadata(path);
  exactKeys(catalog, ["schema_version", "schema_id", "canonical_bytes", "canonical_sha256", "entries"], "fixture catalog");
  if (catalog.schema_version !== "aap-transport-fixture-catalog/0.1" ||
      catalog.schema_id !== TRANSPORT_SCHEMA_ID || !Array.isArray(catalog.entries) ||
      catalog.entries.length !== 102) {
    throw new TypeError("fixture catalog identity or coverage is invalid");
  }
  let canonical = `${catalog.schema_version}\n${catalog.schema_id}\n`;
  let previous = "";
  for (const [index, entry] of catalog.entries.entries()) {
    exactKeys(entry, ["definition", "value_json"], `fixture ${index}`);
    if (typeof entry.definition !== "string" || entry.definition <= previous ||
        typeof entry.value_json !== "string") {
      throw new TypeError(`fixture ${index} metadata is invalid`);
    }
    previous = entry.definition;
    const value = validateTransportDefinitionRaw(entry.definition, entry.value_json);
    const raw = canonicalTransportJson(value);
    canonical += `${entry.definition}\t${Buffer.byteLength(raw, "utf8")}\n${raw}\n`;
  }
  const bytes = Buffer.from(canonical, "utf8");
  const digest = createHash("sha256").update(bytes).digest("hex");
  if (bytes.length !== catalog.canonical_bytes || digest !== catalog.canonical_sha256) {
    throw new TypeError(`fixture serialization differs from golden: computed ${bytes.length} ${digest}`);
  }
  console.log(`${bytes.length} ${digest}`);
};

const probeOracles = new Map([
  ["$probe:anyOf", createTransportOracle({
    $schema: "https://json-schema.org/draft/2020-12/schema",
    $id: "https://aegisy.cc/schemas/aap/oracle-probes/any-of.schema.json",
    $defs: {},
    anyOf: [{ type: "string", minLength: 1 }, { type: "integer", minimum: 1 }],
  })],
  ["$probe:constNumber", createTransportOracle({
    $schema: "https://json-schema.org/draft/2020-12/schema",
    $id: "https://aegisy.cc/schemas/aap/oracle-probes/const-number.schema.json",
    $defs: {},
    const: 1,
  })],
  ["$probe:uniqueItems", createTransportOracle({
    $schema: "https://json-schema.org/draft/2020-12/schema",
    $id: "https://aegisy.cc/schemas/aap/oracle-probes/unique-items.schema.json",
    $defs: {},
    type: "array",
    uniqueItems: true,
    items: true,
  })],
]);

const emitCorpus = (path) => {
  const corpus = readMetadata(path);
  exactKeys(corpus, ["schema_version", "schema_id", "parser_profile", "expected_decisions_sha256", "cases"], "corpus");
  if (corpus.schema_version !== CORPUS_SCHEMA || corpus.schema_id !== TRANSPORT_SCHEMA_ID ||
      corpus.parser_profile !== PARSER_PROFILE || !Array.isArray(corpus.cases) ||
      corpus.cases.length < 1 || corpus.cases.length > 128) {
    throw new TypeError("corpus identity or bounds are invalid");
  }
  const names = new Set();
  const decisions = [];
  for (const [index, entry] of corpus.cases.entries()) {
    exactKeys(entry, ["name", "target", "valid", "value_json"], `case ${index}`);
    if (typeof entry.name !== "string" || names.has(entry.name) ||
        typeof entry.target !== "string" || typeof entry.valid !== "boolean" ||
        typeof entry.value_json !== "string") {
      throw new TypeError(`case ${index} metadata is invalid`);
    }
    names.add(entry.name);
    let accepted = true;
    try {
      if (entry.target === "$root") validateTransportMessageRaw(entry.value_json);
      else if (probeOracles.has(entry.target)) probeOracles.get(entry.target).validateRootRaw(entry.value_json);
      else validateTransportDefinitionRaw(entry.target, entry.value_json);
    } catch {
      accepted = false;
    }
    if (accepted !== entry.valid) throw new TypeError(`case ${entry.name} decision differs from expectation`);
    decisions.push({ name: entry.name, target: entry.target, accepted });
  }
  const digest = stableDecisionIdentity(`${CORPUS_SCHEMA}\t${PARSER_PROFILE}`, decisions);
  if (digest !== corpus.expected_decisions_sha256) {
    throw new TypeError(`corpus decision identity differs from golden: computed ${digest}`);
  }
  console.log(`${decisions.length} ${digest}`);
};

const arguments_ = process.argv.slice(2);
if (arguments_.length === 1) emitFixtures(arguments_[0]);
else if (arguments_.length === 2 && arguments_[0] === "--corpus") emitCorpus(arguments_[1]);
else throw new Error("usage: run-generated-transport-types.mjs [--corpus] <path>");
