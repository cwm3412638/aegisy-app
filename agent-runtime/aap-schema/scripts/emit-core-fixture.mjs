import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import {
  canonicalCoreFixtureCatalog,
  decodeCoreFixtureCatalog,
  validateCoreDefinition,
} from "../generated/typescript/core_types.mjs";

const exactKeys = (value, required, context) => {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    throw new TypeError(`${context} must be an object`);
  }
  const expected = new Set(required);
  for (const key of Object.keys(value)) {
    if (!expected.has(key)) throw new TypeError(`${context} contains unknown field ${key}`);
  }
  for (const key of required) {
    if (!Object.hasOwn(value, key)) throw new TypeError(`${context} is missing ${key}`);
  }
};

const emitCorpusIdentity = (corpusPath) => {
  const corpus = JSON.parse(readFileSync(corpusPath, "utf8"));
  exactKeys(corpus, ["schema_version", "expected_decisions_sha256", "cases"], "corpus");
  if (corpus.schema_version !== "aap-core-generated-corpus/0.1") {
    throw new TypeError("unsupported corpus version");
  }
  if (typeof corpus.expected_decisions_sha256 !== "string" ||
      !/^[0-9a-f]{64}$/.test(corpus.expected_decisions_sha256)) {
    throw new TypeError("corpus decision identity must be a lowercase SHA-256 digest");
  }
  if (!Array.isArray(corpus.cases) || corpus.cases.length < 1 || corpus.cases.length > 128) {
    throw new TypeError("corpus cases must contain 1 through 128 entries");
  }

  const names = new Set();
  let decisions = `${corpus.schema_version}\n`;
  for (const [index, entry] of corpus.cases.entries()) {
    exactKeys(entry, ["name", "definition", "valid", "value_json"], `case ${index}`);
    if (typeof entry.name !== "string" ||
        !/^[a-z0-9]+(?:-[a-z0-9]+)*$/.test(entry.name) || entry.name.length > 96 ||
        names.has(entry.name)) {
      throw new TypeError(`case ${index} has an invalid or duplicate name`);
    }
    names.add(entry.name);
    if (typeof entry.definition !== "string" || !/^[A-Za-z][A-Za-z0-9]*$/.test(entry.definition)) {
      throw new TypeError(`case ${entry.name} has an invalid definition`);
    }
    if (typeof entry.valid !== "boolean") throw new TypeError(`case ${entry.name} has an invalid expectation`);
    if (typeof entry.value_json !== "string" || Buffer.byteLength(entry.value_json, "utf8") > 4 * 1024 * 1024) {
      throw new TypeError(`case ${entry.name} has invalid raw JSON`);
    }

    let accepted = true;
    try {
      validateCoreDefinition(entry.definition, JSON.parse(entry.value_json));
    } catch {
      accepted = false;
    }
    if (accepted !== entry.valid) {
      throw new TypeError(`case ${entry.name} expected ${entry.valid ? "accept" : "reject"} but validator ${accepted ? "accepted" : "rejected"} it`);
    }
    decisions += `${entry.name}\t${entry.definition}\t${accepted ? "accept" : "reject"}\n`;
  }

  const digest = createHash("sha256").update(Buffer.from(decisions, "utf8")).digest("hex");
  if (digest !== corpus.expected_decisions_sha256) {
    throw new TypeError(`corpus decision identity differs from golden: computed ${digest}`);
  }
  console.log(`${corpus.cases.length} ${digest}`);
};

const emitFixtureIdentity = (fixturePath) => {
  const input = JSON.parse(readFileSync(fixturePath, "utf8"));
  const canonical = Buffer.from(canonicalCoreFixtureCatalog(decodeCoreFixtureCatalog(input)));
  const digest = createHash("sha256").update(canonical).digest("hex");
  const fixtureMap = JSON.parse(readFileSync(new URL("../fixtures/aap-core-domains.fixture-map.json", import.meta.url), "utf8"));
  if (canonical.length !== fixtureMap.canonical_bytes || digest !== fixtureMap.canonical_sha256) {
    throw new Error("canonical core fixture identity differs from the reviewed golden identity");
  }
  console.log(`${canonical.length} ${digest}`);
};

const arguments_ = process.argv.slice(2);
if (arguments_.length === 2 && arguments_[0] === "--corpus") {
  emitCorpusIdentity(arguments_[1]);
} else if (arguments_.length === 1) {
  emitFixtureIdentity(arguments_[0]);
} else {
  throw new Error("usage: emit-core-fixture.mjs [--corpus] <path>");
}
