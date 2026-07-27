import { createHash } from "node:crypto";
import { readFileSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";
import {
  canonicalTransportJson,
  createTransportOracle,
  MAX_RAW_JSON_BYTES,
  parseTransportJson,
} from "./transport-schema-oracle.mjs";

const readMetadataJsonStrict = (path) => {
  const bytes = readFileSync(path);
  if (bytes.length > MAX_RAW_JSON_BYTES) throw new TypeError(`${path} exceeds the 4 MiB metadata limit`);
  const raw = new TextDecoder("utf-8", { fatal: true }).decode(bytes);
  parseTransportJson(raw);
  return JSON.parse(raw);
};

const exactKeys = (value, required, optional, context) => {
  if (!value || typeof value !== "object" || Array.isArray(value)) throw new TypeError(`${context} must be an object`);
  const allowed = new Set([...required, ...optional]);
  for (const key of Object.keys(value)) if (!allowed.has(key)) throw new TypeError(`${context} contains unknown field ${key}`);
  for (const key of required) if (!Object.hasOwn(value, key)) throw new TypeError(`${context} is missing ${key}`);
};

const arguments_ = process.argv.slice(2);
if (arguments_.length < 3 || arguments_.length > 4) {
  throw new Error("usage: materialize-transport-fixtures.mjs <schema> <method-registry> <fixture-catalog> [output]");
}

const [schemaPath, methodRegistryPath, fixtureCatalogPath, outputPath] = arguments_.map((argument) => resolve(argument));
if (outputPath && [schemaPath, methodRegistryPath, fixtureCatalogPath].includes(outputPath)) {
  throw new TypeError("materialized output must not replace an input file");
}
const schema = readMetadataJsonStrict(schemaPath);
const oracle = createTransportOracle(schema);
const methodRegistry = readMetadataJsonStrict(methodRegistryPath);
const catalog = readMetadataJsonStrict(fixtureCatalogPath);

exactKeys(methodRegistry, ["schema_version", "schema_id", "response_dispatch", "generic_payload_semantics", "unknown_method_fallbacks", "methods"], [], "method registry");
if (methodRegistry.schema_version !== "aap-transport-method-registry/0.1" || methodRegistry.schema_id !== schema.$id) {
  throw new TypeError("method registry version or schema identity mismatch");
}
exactKeys(methodRegistry.response_dispatch, ["success", "error", "unmatched"], [], "response dispatch");
if (methodRegistry.response_dispatch.success !== "pending-request-method" ||
    methodRegistry.response_dispatch.error !== "pending-request-method-with-typed-error-fail-closed" ||
    methodRegistry.response_dispatch.unmatched !== "generic-root-envelope-only") {
  throw new TypeError("method registry response dispatch policy mismatch");
}
exactKeys(methodRegistry.generic_payload_semantics, ["result", "error_data", "error_code"], [], "generic payload semantics");
if (methodRegistry.generic_payload_semantics.result !== "draft-2020-12-true-schema" ||
    methodRegistry.generic_payload_semantics.error_data !== "draft-2020-12-true-schema" ||
    methodRegistry.generic_payload_semantics.error_code !== "unbounded-mathematical-integer") {
  throw new TypeError("method registry generic payload semantics mismatch");
}
if (!Array.isArray(methodRegistry.unknown_method_fallbacks) || methodRegistry.unknown_method_fallbacks.length !== 2) {
  throw new TypeError("method registry must declare both unknown-method fallbacks");
}
for (const [index, fallback] of methodRegistry.unknown_method_fallbacks.entries()) {
  exactKeys(fallback, ["kind", "selection", "params", "response_dispatch", "typed"], [], `unknown-method fallback ${index}`);
  const expected = index === 0
    ? { kind: "request", selection: "valid-root-request-with-method-not-listed-below", params: "object", response_dispatch: "pending-request-context", typed: false }
    : { kind: "notification", selection: "valid-root-notification-with-method-not-listed-below", params: "object", response_dispatch: "none", typed: false };
  if (Object.entries(expected).some(([key, value]) => fallback[key] !== value)) {
    throw new TypeError(`unknown-method fallback ${index} policy mismatch`);
  }
}
if (!Array.isArray(methodRegistry.methods) || methodRegistry.methods.length < 1 || methodRegistry.methods.length > 64) {
  throw new TypeError("method registry must contain 1 through 64 methods");
}

const schemaMethods = new Map();
for (const condition of schema.allOf ?? []) {
  const method = condition?.if?.properties?.method?.const;
  if (typeof method !== "string") continue;
  if (schemaMethods.has(method)) throw new TypeError(`schema repeats method dispatch ${method}`);
  const notification = condition?.then?.not?.required?.includes("id") === true;
  const paramsReference = condition?.then?.properties?.params?.$ref ?? null;
  schemaMethods.set(method, {
    kind: notification ? "notification" : "request",
    params_definition: paramsReference?.startsWith("#/$defs/") ? paramsReference.slice(8) : null,
  });
}

const registeredMethods = new Set();
let previousMethod = "";
for (const [index, entry] of methodRegistry.methods.entries()) {
  exactKeys(
    entry,
    ["method", "kind", "params_definition", "request_definition", "success_response_definition", "error_response_definitions", "notification_definition"],
    [],
    `method registry entry ${index}`,
  );
  if (typeof entry.method !== "string" || registeredMethods.has(entry.method)) throw new TypeError(`invalid or duplicate method at entry ${index}`);
  if (entry.method <= previousMethod) throw new TypeError("method registry must be strictly sorted by method");
  previousMethod = entry.method;
  registeredMethods.add(entry.method);
  const expected = schemaMethods.get(entry.method);
  if (!expected || entry.kind !== expected.kind || entry.params_definition !== expected.params_definition) {
    throw new TypeError(`method registry dispatch mismatch for ${entry.method}`);
  }
  for (const field of ["request_definition", "success_response_definition", "notification_definition"]) {
    const definition = entry[field];
    if (definition !== null && (typeof definition !== "string" || !oracle.definitionNames.includes(definition))) {
      throw new TypeError(`${entry.method} has unknown ${field}`);
    }
  }
  if (!Array.isArray(entry.error_response_definitions) ||
      entry.error_response_definitions.some((definition) => typeof definition !== "string" || !oracle.definitionNames.includes(definition))) {
    throw new TypeError(`${entry.method} has invalid error response definitions`);
  }
  if (entry.kind === "request" && (entry.request_definition === null || entry.success_response_definition === null || entry.notification_definition !== null)) {
    throw new TypeError(`${entry.method} request bindings are incomplete`);
  }
  if (entry.kind === "notification" && (entry.request_definition !== null || entry.success_response_definition !== null || entry.error_response_definitions.length !== 0)) {
    throw new TypeError(`${entry.method} notification bindings are incomplete`);
  }
}
if (registeredMethods.size !== schemaMethods.size || [...schemaMethods.keys()].some((method) => !registeredMethods.has(method))) {
  throw new TypeError("method registry does not exactly cover root method dispatch");
}

exactKeys(catalog, ["schema_version", "schema_id", "canonical_bytes", "canonical_sha256", "entries"], [], "fixture catalog");
if (catalog.schema_version !== "aap-transport-fixture-catalog/0.1" || catalog.schema_id !== schema.$id) {
  throw new TypeError("fixture catalog version or schema identity mismatch");
}
if (!Number.isSafeInteger(catalog.canonical_bytes) || catalog.canonical_bytes < 1 ||
    typeof catalog.canonical_sha256 !== "string" || !/^[0-9a-f]{64}$/.test(catalog.canonical_sha256)) {
  throw new TypeError("fixture catalog has an invalid canonical identity");
}
if (!Array.isArray(catalog.entries) || catalog.entries.length !== oracle.definitionNames.length) {
  throw new TypeError(`fixture catalog must contain exactly ${oracle.definitionNames.length} entries`);
}

const entries = new Map();
for (const [index, entry] of catalog.entries.entries()) {
  exactKeys(entry, ["definition", "value_json"], [], `fixture entry ${index}`);
  if (typeof entry.definition !== "string" || !oracle.definitionNames.includes(entry.definition) || entries.has(entry.definition)) {
    throw new TypeError(`fixture entry ${index} has an unknown or duplicate definition`);
  }
  if (entry.definition !== oracle.definitionNames[index]) throw new TypeError("fixture entries must be sorted by definition");
  if (typeof entry.value_json !== "string") throw new TypeError(`fixture ${entry.definition} value_json must be a string`);
  const value = oracle.validateDefinitionRaw(entry.definition, entry.value_json);
  entries.set(entry.definition, canonicalTransportJson(value));
}
if (oracle.definitionNames.some((definition) => !entries.has(definition))) throw new TypeError("fixture catalog has incomplete definition coverage");

for (const method of methodRegistry.methods) {
  for (const definition of [method.request_definition, method.success_response_definition,
    method.notification_definition, ...method.error_response_definitions]) {
    if (definition !== null) oracle.validateRootRaw(entries.get(definition));
  }
}

let canonical = `${catalog.schema_version}\n${catalog.schema_id}\n`;
for (const definition of oracle.definitionNames) {
  const raw = entries.get(definition);
  canonical += `${definition}\t${Buffer.byteLength(raw, "utf8")}\n${raw}\n`;
}
const canonicalBuffer = Buffer.from(canonical, "utf8");
const digest = createHash("sha256").update(canonicalBuffer).digest("hex");
if (canonicalBuffer.length !== catalog.canonical_bytes || digest !== catalog.canonical_sha256) {
  throw new TypeError(`fixture catalog identity differs from golden: computed ${canonicalBuffer.length} ${digest}`);
}

if (outputPath) writeFileSync(outputPath, canonicalBuffer);
console.log(`${canonicalBuffer.length} ${digest}`);
