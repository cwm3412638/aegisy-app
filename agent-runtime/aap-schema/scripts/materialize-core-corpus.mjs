import { readFileSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";

const [corpusArgument, fixtureArgument, outputArgument] = process.argv.slice(2);
if (!corpusArgument || !fixtureArgument || !outputArgument) {
  throw new Error("usage: materialize-core-corpus.mjs <corpus> <fixture-catalog> <output>");
}

const corpus = JSON.parse(readFileSync(resolve(corpusArgument), "utf8"));
const fixtureCatalog = JSON.parse(readFileSync(resolve(fixtureArgument), "utf8"));
const clone = (value) => JSON.parse(JSON.stringify(value));

const exactKeys = (value, required, optional, context) => {
  if (!value || typeof value !== "object" || Array.isArray(value)) throw new Error(`${context} must be an object`);
  const allowed = new Set([...required, ...optional]);
  for (const key of Object.keys(value)) if (!allowed.has(key)) throw new Error(`${context} contains unknown field ${key}`);
  for (const key of required) if (!Object.hasOwn(value, key)) throw new Error(`${context} is missing ${key}`);
};

exactKeys(corpus, ["schema_version", "expected_decisions_sha256", "cases"], [], "corpus");
if (corpus.schema_version !== "aap-core-generated-corpus/0.1") throw new Error("unsupported corpus version");
if (typeof corpus.expected_decisions_sha256 !== "string" || !/^[0-9a-f]{64}$/.test(corpus.expected_decisions_sha256)) {
  throw new Error("corpus decision identity must be a lowercase SHA-256 digest");
}
if (!Array.isArray(corpus.cases) || corpus.cases.length < 1 || corpus.cases.length > 128) {
  throw new Error("corpus cases must contain 1 through 128 entries");
}

const caseNames = new Set();
const materialized = corpus.cases.map((entry, caseIndex) => {
  exactKeys(entry, ["name", "definition", "source", "valid", "mutations"], ["source_index", "source_path"], `case ${caseIndex}`);
  if (typeof entry.name !== "string" || !/^[a-z0-9]+(?:-[a-z0-9]+)*$/.test(entry.name) || entry.name.length > 96) {
    throw new Error(`case ${caseIndex} has an invalid name`);
  }
  if (caseNames.has(entry.name)) throw new Error(`duplicate corpus case ${entry.name}`);
  caseNames.add(entry.name);
  if (typeof entry.definition !== "string" || !/^[A-Za-z][A-Za-z0-9]*$/.test(entry.definition)) {
    throw new Error(`case ${entry.name} has an invalid definition`);
  }
  if (typeof entry.source !== "string" || !Object.hasOwn(fixtureCatalog, entry.source)) {
    throw new Error(`case ${entry.name} has an unknown fixture source`);
  }
  if (typeof entry.valid !== "boolean" || !Array.isArray(entry.mutations) || entry.mutations.length > 16) {
    throw new Error(`case ${entry.name} has invalid expectation or mutations`);
  }
  let value = clone(fixtureCatalog[entry.source]);
  if (Object.hasOwn(entry, "source_index")) {
    if (!Number.isSafeInteger(entry.source_index) || entry.source_index < 0 || !Array.isArray(value) || entry.source_index >= value.length) {
      throw new Error(`case ${entry.name} has an invalid source index`);
    }
    value = value[entry.source_index];
  }
  if (Object.hasOwn(entry, "source_path")) {
    if (!Array.isArray(entry.source_path) || entry.source_path.length < 1 || entry.source_path.length > 32 ||
        entry.source_path.some((part) => !(typeof part === "string" || (Number.isSafeInteger(part) && part >= 0)))) {
      throw new Error(`case ${entry.name} has an invalid source path`);
    }
    for (const part of entry.source_path) {
      if (!value || typeof value !== "object" || !Object.hasOwn(value, part)) {
        throw new Error(`case ${entry.name} source path is missing`);
      }
      value = value[part];
    }
  }
  for (const [mutationIndex, mutation] of entry.mutations.entries()) {
    exactKeys(
      mutation,
      ["op", "path"],
      ["value", "depth", "count"],
      `case ${entry.name} mutation ${mutationIndex}`,
    );
    if (!Array.isArray(mutation.path) || mutation.path.length > 32 ||
        mutation.path.some((part) => !(typeof part === "string" || (Number.isSafeInteger(part) && part >= 0)))) {
      throw new Error(`case ${entry.name} mutation ${mutationIndex} has an invalid path`);
    }
    if (mutation.path.length === 0) {
      if (mutation.op !== "set" || !Object.hasOwn(mutation, "value")) {
        throw new Error(`case ${entry.name} root mutation must be set with a value`);
      }
      value = clone(mutation.value);
      continue;
    }
    let parent = value;
    for (const part of mutation.path.slice(0, -1)) {
      if (!parent || typeof parent !== "object" || !Object.hasOwn(parent, part)) {
        throw new Error(`case ${entry.name} mutation ${mutationIndex} path is missing`);
      }
      parent = parent[part];
    }
    const leaf = mutation.path.at(-1);
    if (!parent || typeof parent !== "object") throw new Error(`case ${entry.name} mutation ${mutationIndex} parent is invalid`);
    if (mutation.op === "set") {
      if (!Object.hasOwn(mutation, "value") || Object.hasOwn(mutation, "depth") || Object.hasOwn(mutation, "count")) {
        throw new Error(`case ${entry.name} set mutation is invalid`);
      }
      parent[leaf] = clone(mutation.value);
    } else if (mutation.op === "remove") {
      if (Object.hasOwn(mutation, "value") || Object.hasOwn(mutation, "depth") ||
          Object.hasOwn(mutation, "count") || !Object.hasOwn(parent, leaf)) {
        throw new Error(`case ${entry.name} remove mutation is invalid`);
      }
      if (Array.isArray(parent)) parent.splice(leaf, 1);
      else delete parent[leaf];
    } else if (mutation.op === "set-nested-array") {
      if (Object.hasOwn(mutation, "value") || Object.hasOwn(mutation, "count") ||
          !Number.isSafeInteger(mutation.depth) || mutation.depth < 0 || mutation.depth > 32) {
        throw new Error(`case ${entry.name} nested-array mutation is invalid`);
      }
      let nested = null;
      for (let depth = 0; depth < mutation.depth; depth += 1) nested = [nested];
      parent[leaf] = nested;
    } else if (mutation.op === "set-filled-array") {
      if (Object.hasOwn(mutation, "value") || Object.hasOwn(mutation, "depth") ||
          !Number.isSafeInteger(mutation.count) || mutation.count < 0 || mutation.count > 5000) {
        throw new Error(`case ${entry.name} filled-array mutation is invalid`);
      }
      parent[leaf] = Array.from({ length: mutation.count }, () => null);
    } else {
      throw new Error(`case ${entry.name} has unsupported mutation ${mutation.op}`);
    }
  }
  const valueJson = JSON.stringify(value);
  if (typeof valueJson !== "string") throw new Error(`case ${entry.name} cannot be represented as JSON`);
  return { name: entry.name, definition: entry.definition, valid: entry.valid, value_json: valueJson };
});

writeFileSync(resolve(outputArgument), `${JSON.stringify({
  schema_version: corpus.schema_version,
  expected_decisions_sha256: corpus.expected_decisions_sha256,
  cases: materialized,
})}\n`);
