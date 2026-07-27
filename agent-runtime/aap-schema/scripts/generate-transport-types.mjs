import { createHash } from "node:crypto";
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, relative, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
let schemaPath = resolve(packageRoot, "stable/v0.1/aap.schema.json");
let methodRegistryPath = resolve(packageRoot, "fixtures/aap-transport-methods.json");
let checkOnly = false;
let validateRegistryCandidate = false;
for (let index = 2; index < process.argv.length; index += 1) {
  const argument = process.argv[index];
  if (argument === "--check") checkOnly = true;
  else if (argument === "--validate-registry-candidate") validateRegistryCandidate = true;
  else if (argument === "--schema") {
    const value = process.argv[++index];
    if (!value) throw new Error("--schema requires a path");
    schemaPath = resolve(value);
  } else if (argument === "--method-registry") {
    const value = process.argv[++index];
    if (!value) throw new Error("--method-registry requires a path");
    methodRegistryPath = resolve(value);
  } else throw new Error(`unsupported generator argument ${argument}`);
}
if (validateRegistryCandidate && !checkOnly) {
  throw new Error("--validate-registry-candidate requires --check");
}

const schemaBytes = readFileSync(schemaPath);
const schema = JSON.parse(schemaBytes);
const expectedSchemaId = "https://aegisy.cc/schemas/aap/stable/v0.1/aap.schema.json";
if (schema.$schema !== "https://json-schema.org/draft/2020-12/schema" ||
    schema.$id !== expectedSchemaId || !schema.$defs || Array.isArray(schema.$defs)) {
  throw new Error("transport generator requires the stable AAP 0.1 Draft 2020-12 schema");
}
const schemaSha256 = createHash("sha256").update(schemaBytes).digest("hex");
const definitions = schema.$defs;
const methodRegistryBytes = readFileSync(methodRegistryPath);
const methodRegistry = JSON.parse(methodRegistryBytes);
const methodRegistrySha256 = createHash("sha256").update(methodRegistryBytes).digest("hex");
const expectedMethodRegistrySha256 = "b90f2572b61f4e6c75548b5655cd2374b469231e282e5dd1a3e6f9f9da09953c";
if (!validateRegistryCandidate && methodRegistrySha256 !== expectedMethodRegistrySha256) {
  throw new Error(`transport method registry differs from reviewed golden ${expectedMethodRegistrySha256}`);
}

const supportedKeywords = new Set([
  "$comment", "$defs", "$id", "$ref", "$schema", "additionalProperties",
  "allOf", "anyOf", "const", "else", "enum", "if", "items", "maximum",
  "maxItems", "maxLength", "maxProperties", "minimum", "minItems",
  "minLength", "not", "oneOf", "pattern", "properties", "propertyNames",
  "required", "then", "title", "type", "uniqueItems",
]);
const audit = (node, context, root = false) => {
  if (node === true || node === false) return;
  if (!node || typeof node !== "object" || Array.isArray(node)) {
    throw new Error(`${context} must be an object or boolean schema`);
  }
  for (const keyword of Object.keys(node)) {
    if (!supportedKeywords.has(keyword)) throw new Error(`${context} uses unsupported keyword ${keyword}`);
  }
  for (const keyword of ["$schema", "$id", "$defs"]) {
    if (!root && Object.hasOwn(node, keyword)) throw new Error(`${context} uses nested ${keyword}`);
  }
  if (Object.hasOwn(node, "$ref")) {
    if (typeof node.$ref !== "string" || !node.$ref.startsWith("#/$defs/") ||
        !Object.hasOwn(definitions, node.$ref.slice(8))) {
      throw new Error(`${context} uses an unsupported reference`);
    }
  }
  if (Object.hasOwn(node, "type") &&
      !["null", "boolean", "integer", "string", "array", "object"].includes(node.type)) {
    throw new Error(`${context} uses unsupported type ${String(node.type)}`);
  }
  for (const keyword of ["minimum", "maximum", "minItems", "maxItems", "minLength",
    "maxLength", "minProperties", "maxProperties"]) {
    if (Object.hasOwn(node, keyword) && !Number.isSafeInteger(node[keyword])) {
      throw new Error(`${context}.${keyword} must be a JSON-safe integer`);
    }
  }
  for (const keyword of ["allOf", "anyOf", "oneOf"]) {
    if (!Object.hasOwn(node, keyword)) continue;
    if (!Array.isArray(node[keyword]) || node[keyword].length === 0) {
      throw new Error(`${context}.${keyword} must be a non-empty array`);
    }
    node[keyword].forEach((child, index) => audit(child, `${context}.${keyword}[${index}]`));
  }
  for (const keyword of ["if", "then", "else", "not", "items", "propertyNames"]) {
    if (Object.hasOwn(node, keyword)) audit(node[keyword], `${context}.${keyword}`);
  }
  if (Object.hasOwn(node, "additionalProperties") && typeof node.additionalProperties !== "boolean") {
    audit(node.additionalProperties, `${context}.additionalProperties`);
  }
  for (const keyword of ["properties", "$defs"]) {
    if (!Object.hasOwn(node, keyword)) continue;
    if (!node[keyword] || typeof node[keyword] !== "object" || Array.isArray(node[keyword])) {
      throw new Error(`${context}.${keyword} must be an object`);
    }
    for (const [name, child] of Object.entries(node[keyword])) audit(child, `${context}.${keyword}.${name}`);
  }
};
audit(schema, "transport schema", true);

const compare = (left, right) => left < right ? -1 : left > right ? 1 : 0;
const exactKeys = (value, keys, context) => {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    throw new Error(`${context} must be an object`);
  }
  const expected = new Set(keys);
  for (const key of Object.keys(value)) {
    if (!expected.has(key)) throw new Error(`${context} contains unknown field ${key}`);
  }
  for (const key of keys) {
    if (!Object.hasOwn(value, key)) throw new Error(`${context} is missing ${key}`);
  }
};

exactKeys(methodRegistry, ["schema_version", "schema_id", "response_dispatch",
  "generic_payload_semantics", "unknown_method_fallbacks", "methods"], "transport method registry");
if (methodRegistry.schema_version !== "aap-transport-method-registry/0.1" ||
    methodRegistry.schema_id !== expectedSchemaId || !Array.isArray(methodRegistry.methods) ||
    methodRegistry.methods.length < 1 || methodRegistry.methods.length > 64) {
  throw new Error("transport method registry identity or method count is invalid");
}
exactKeys(methodRegistry.response_dispatch, ["success", "error", "unmatched"], "transport response dispatch");
if (methodRegistry.response_dispatch.success !== "pending-request-method" ||
    methodRegistry.response_dispatch.error !== "pending-request-method-with-typed-error-fail-closed" ||
    methodRegistry.response_dispatch.unmatched !== "generic-root-envelope-only") {
  throw new Error("transport response dispatch policy is invalid");
}
exactKeys(methodRegistry.generic_payload_semantics, ["result", "error_data", "error_code"],
  "transport generic payload semantics");
if (methodRegistry.generic_payload_semantics.result !== "draft-2020-12-true-schema" ||
    methodRegistry.generic_payload_semantics.error_data !== "draft-2020-12-true-schema" ||
    methodRegistry.generic_payload_semantics.error_code !== "unbounded-mathematical-integer") {
  throw new Error("transport generic payload semantics are invalid");
}
if (!Array.isArray(methodRegistry.unknown_method_fallbacks) ||
    JSON.stringify(methodRegistry.unknown_method_fallbacks) !== JSON.stringify([
      { kind: "request", selection: "valid-root-request-with-method-not-listed-below", params: "object", response_dispatch: "pending-request-context", typed: false },
      { kind: "notification", selection: "valid-root-notification-with-method-not-listed-below", params: "object", response_dispatch: "none", typed: false },
    ])) {
  throw new Error("transport unknown-method fallback policy is invalid");
}

const schemaMethods = new Map();
const consumedRootConditionIndexes = new Set();
for (const [index, condition] of (schema.allOf ?? []).entries()) {
  const method = condition?.if?.properties?.method?.const;
  if (typeof method !== "string") continue;
  if (schemaMethods.has(method) || condition?.if?.required?.includes("method") !== true) {
    throw new Error(`transport schema method dispatch ${method} is duplicated or not method-bound`);
  }
  const notification = condition?.then?.not?.required?.includes("id") === true;
  const paramsReference = condition?.then?.properties?.params?.$ref ?? null;
  schemaMethods.set(method, {
    kind: notification ? "notification" : "request",
    params_definition: typeof paramsReference === "string" && paramsReference.startsWith("#/$defs/")
      ? paramsReference.slice(8)
      : null,
    condition_index: index,
  });
  consumedRootConditionIndexes.add(index);
}

const definitionExists = (name) => typeof name === "string" && Object.hasOwn(definitions, name);
const typedErrorsBySchemaVersion = new Map();
const referenceDefinition = (node) => typeof node?.$ref === "string" && node.$ref.startsWith("#/$defs/")
  ? node.$ref.slice(8)
  : null;
const hasRequired = (node, ...fields) => Array.isArray(node?.required) &&
  fields.every((field) => node.required.includes(field));
const sameSchema = (left, right) => JSON.stringify(left) === JSON.stringify(right);
const schemaVersionDiscriminatorFacts = (node, seen = new Set()) => {
  const facts = { values: new Set(), required: false };
  if (!node || typeof node !== "object" || Array.isArray(node)) return facts;
  if (typeof node.properties?.schema_version?.const === "string") {
    facts.values.add(node.properties.schema_version.const);
  }
  facts.required = node.required?.includes("schema_version") === true;
  const candidates = [...(node.allOf ?? [])];
  if (typeof node.$ref === "string") candidates.push({ $ref: node.$ref });
  for (const candidate of candidates) {
    const name = referenceDefinition(candidate);
    let nested;
    if (name) {
      if (seen.has(name)) continue;
      const nextSeen = new Set(seen);
      nextSeen.add(name);
      nested = schemaVersionDiscriminatorFacts(definitions[name], nextSeen);
    } else {
      nested = schemaVersionDiscriminatorFacts(candidate, seen);
    }
    for (const value of nested.values) facts.values.add(value);
    facts.required ||= nested.required;
  }
  return facts;
};
const typedErrorDiscriminator = (definitionName) => {
  const definition = definitions[definitionName];
  const dataDefinitionName = referenceDefinition(definition?.properties?.error?.properties?.data);
  if (!dataDefinitionName) {
    throw new Error(`typed error ${definitionName} does not bind error.data to a definition`);
  }
  const facts = schemaVersionDiscriminatorFacts(
    definitions[dataDefinitionName],
    new Set([dataDefinitionName]),
  );
  const schemaVersions = [...facts.values];
  if (!facts.required || schemaVersions.length !== 1 || schemaVersions[0].length < 1) {
    throw new Error(`typed error ${definitionName} has no required unique schema_version discriminator`);
  }
  return schemaVersions[0];
};

const registeredMethods = new Set();
let previousMethod = "";
for (const [index, entry] of methodRegistry.methods.entries()) {
  exactKeys(entry, ["method", "kind", "params_definition", "request_definition",
    "success_response_definition", "success_result_definition", "error_response_definitions",
    "typed_error_stage", "notification_definition"],
  `transport method registry entry ${index}`);
  if (typeof entry.method !== "string" || entry.method <= previousMethod || registeredMethods.has(entry.method)) {
    throw new Error("transport method registry methods must be unique and strictly sorted");
  }
  previousMethod = entry.method;
  registeredMethods.add(entry.method);
  const expected = schemaMethods.get(entry.method);
  if (!expected || entry.kind !== expected.kind || entry.params_definition !== expected.params_definition) {
    throw new Error(`transport method registry dispatch mismatch for ${entry.method}`);
  }
  for (const field of ["params_definition", "request_definition", "success_response_definition",
    "success_result_definition", "notification_definition"]) {
    const definition = entry[field];
    if (definition !== null && !definitionExists(definition)) {
      throw new Error(`${entry.method} has unknown ${field}`);
    }
  }
  if (!Array.isArray(entry.error_response_definitions)) {
    throw new Error(`${entry.method} has invalid error response definitions`);
  }
  const expectedTypedErrorStages = new Map([
    ["timeline/subscribe", "subscribe"],
    ["timeline/subscription-sync", "sync"],
    ["timeline/subscription-snapshot", "snapshot"],
    ["timeline/subscription-activate", "activate"],
  ]);
  if (entry.typed_error_stage !== (expectedTypedErrorStages.get(entry.method) ?? null)) {
    throw new Error(`${entry.method} typed error stage is invalid`);
  }
  let previousError = "";
  for (const definition of entry.error_response_definitions) {
    if (!definitionExists(definition) || definition <= previousError) {
      throw new Error(`${entry.method} error response definitions must be known, unique, and sorted`);
    }
    previousError = definition;
    const schemaVersion = typedErrorDiscriminator(definition);
    const existingDefinition = typedErrorsBySchemaVersion.get(schemaVersion);
    if (existingDefinition && existingDefinition !== definition) {
      throw new Error(`typed error discriminator ${schemaVersion} is ambiguous`);
    }
    typedErrorsBySchemaVersion.set(schemaVersion, definition);
  }
  if (entry.kind === "request" &&
      (!definitionExists(entry.request_definition) || !definitionExists(entry.success_response_definition) ||
       !definitionExists(entry.success_result_definition) ||
       entry.notification_definition !== null)) {
    throw new Error(`${entry.method} request bindings are incomplete`);
  }
  if (entry.kind === "notification" &&
      (entry.request_definition !== null || entry.success_response_definition !== null ||
       entry.success_result_definition !== null ||
       entry.error_response_definitions.length !== 0)) {
    throw new Error(`${entry.method} notification bindings are invalid`);
  }

  if (entry.request_definition !== null) {
    const request = definitions[entry.request_definition];
    if (request?.type !== "object" || request?.additionalProperties !== false ||
        !hasRequired(request, "jsonrpc", "id", "method", "params") ||
        request?.properties?.jsonrpc?.const !== "2.0" ||
        referenceDefinition(request?.properties?.id) !== "jsonRpcRequestId" ||
        request?.properties?.method?.const !== entry.method ||
        referenceDefinition(request?.properties?.params) !== entry.params_definition) {
      throw new Error(`${entry.method} request definition does not bind its method and params`);
    }
  }
  if (entry.success_response_definition !== null) {
    const success = definitions[entry.success_response_definition];
    if (success?.type !== "object" || success?.additionalProperties !== false ||
        !hasRequired(success, "jsonrpc", "id", "result") ||
        success?.properties?.jsonrpc?.const !== "2.0" ||
        referenceDefinition(success?.properties?.id) !== "jsonRpcRequestId" ||
        referenceDefinition(success?.properties?.result) !== entry.success_result_definition ||
        Object.hasOwn(success?.properties ?? {}, "method") ||
        Object.hasOwn(success?.properties ?? {}, "error")) {
      throw new Error(`${entry.method} success response definition is not a strict success envelope`);
    }
  }
  if (entry.notification_definition !== null) {
    const notification = definitions[entry.notification_definition];
    const condition = schema.allOf[expected.condition_index];
    if (notification?.type !== "object" || notification?.additionalProperties !== false ||
        !hasRequired(notification, "jsonrpc", "method", "params") ||
        notification?.properties?.jsonrpc?.const !== "2.0" ||
        notification?.properties?.method?.const !== entry.method ||
        Object.hasOwn(notification?.properties ?? {}, "id") ||
        !sameSchema(notification?.properties?.params, condition?.then?.properties?.params)) {
      throw new Error(`${entry.method} notification definition does not bind its method and params`);
    }
  }
  for (const definitionName of entry.error_response_definitions) {
    const response = definitions[definitionName];
    const error = response?.properties?.error;
    const errorProperties = error?.properties;
    const errorReference = referenceDefinition(error);
    const referencedError = errorReference ? definitions[errorReference] : null;
    const effectiveErrorType = error?.type ?? referencedError?.type;
    const effectiveAdditionalProperties = error?.additionalProperties ??
      referencedError?.additionalProperties;
    const effectiveCode = errorProperties?.code ?? referencedError?.properties?.code;
    const effectiveMessage = errorProperties?.message ?? referencedError?.properties?.message;
    const effectiveData = errorProperties?.data;
    if (response?.type !== "object" || response?.additionalProperties !== false ||
        !hasRequired(response, "jsonrpc", "id", "error") ||
        response?.properties?.jsonrpc?.const !== "2.0" ||
        referenceDefinition(response?.properties?.id) !== "jsonRpcRequestId" ||
        !hasRequired(error, "code", "message", "data") ||
        effectiveErrorType !== "object" || effectiveAdditionalProperties !== false ||
        !effectiveCode || !effectiveMessage || !effectiveData ||
        (!["integer", undefined].includes(effectiveCode.type) &&
         !Object.hasOwn(effectiveCode, "const")) ||
        effectiveMessage.type !== "string" ||
        referenceDefinition(effectiveData) === null) {
      throw new Error(`${entry.method} typed error definition is not a strict error envelope`);
    }
  }
}
if (registeredMethods.size !== schemaMethods.size ||
    [...schemaMethods.keys()].some((method) => !registeredMethods.has(method))) {
  throw new Error("transport method registry does not exactly cover root method dispatch");
}
if (!Array.isArray(schema.allOf)) {
  throw new Error("transport schema must declare root allOf dispatch conditions");
}
const typedErrorMetadata = methodRegistry.methods
  .flatMap((entry) => entry.error_response_definitions.map((definition) => ({
    method: entry.method,
    schema_version: typedErrorDiscriminator(definition),
    response_definition: definition,
  })))
  .sort((left, right) => compare(left.method, right.method) ||
    compare(left.schema_version, right.schema_version));
const typedErrorSchemaVersions = [...typedErrorsBySchemaVersion.keys()].sort(compare);

// Every root allOf condition is deliberately removed from the generic envelope
// validator, so generation must prove that dispatch restores each one.
for (const [index, condition] of schema.allOf.entries()) {
  if (consumedRootConditionIndexes.has(index)) continue;
  const discriminator = condition?.if?.properties?.error?.properties?.data?.properties
    ?.schema_version?.const;
  const dataDefinition = referenceDefinition(condition?.then?.properties?.error?.properties?.data);
  const matchingResponses = typedErrorMetadata.filter((metadata) =>
    metadata.schema_version === discriminator &&
    referenceDefinition(definitions[metadata.response_definition]?.properties?.error?.properties?.data) ===
      dataDefinition);
  if (typeof discriminator !== "string" ||
      !hasRequired(condition?.if, "error") ||
      !hasRequired(condition?.if?.properties?.error, "data") ||
      !hasRequired(condition?.if?.properties?.error?.properties?.data, "schema_version") ||
      referenceDefinition(condition?.then?.properties?.id) !== "jsonRpcRequestId" ||
      !dataDefinition || matchingResponses.length === 0) {
    throw new Error(`transport root allOf condition ${index} is not covered by method or typed-error dispatch`);
  }
  consumedRootConditionIndexes.add(index);
}
if (consumedRootConditionIndexes.size !== schema.allOf.length) {
  throw new Error("transport root allOf dispatch coverage is incomplete");
}

const pascal = (value) => value
  .replace(/[^A-Za-z0-9]+(.)?/g, (_match, next) => next ? next.toUpperCase() : "")
  .replace(/^[a-z]/, (first) => first.toUpperCase());
const snake = (value) => value
  .replace(/([a-z0-9])([A-Z])/g, "$1_$2")
  .replace(/[^A-Za-z0-9]+/g, "_")
  .replace(/^_+|_+$/g, "")
  .toLowerCase();
const rustReserved = new Set(["as", "async", "await", "break", "const", "continue", "crate",
  "dyn", "else", "enum", "extern", "false", "fn", "for", "if", "impl", "in", "let",
  "loop", "match", "mod", "move", "pub", "ref", "return", "self", "static", "struct",
  "super", "trait", "true", "type", "unsafe", "use", "where", "while"]);
const cppReserved = new Set(["alignas", "alignof", "and", "asm", "auto", "bool", "break",
  "case", "catch", "char", "class", "const", "constexpr", "continue", "default", "delete",
  "do", "double", "else", "enum", "explicit", "export", "extern", "false", "float", "for",
  "friend", "goto", "if", "inline", "int", "long", "namespace", "new", "noexcept", "not",
  "nullptr", "operator", "or", "private", "protected", "public", "return", "short", "signed",
  "sizeof", "static", "struct", "switch", "template", "this", "throw", "true", "try", "typedef",
  "typename", "union", "unsigned", "using", "virtual", "void", "volatile", "while"]);
const rustField = (value) => rustReserved.has(value) ? `${value}_value` : value;
const cppField = (value) => cppReserved.has(value) ? `${value}_value` : value;
const enumVariant = (value) => {
  const result = pascal(String(value));
  return /^[0-9]/.test(result) ? `Value${result}` : result || "Value";
};

const typeNames = new Map();
const namesByType = new Map();
for (const name of Object.keys(definitions)) {
  const generated = pascal(name);
  if (!/^[A-Za-z][A-Za-z0-9]*$/.test(generated) || namesByType.has(generated)) {
    throw new Error(`definition name collision at ${name}`);
  }
  typeNames.set(name, generated);
  namesByType.set(generated, name);
}
const refType = (reference) => {
  const name = reference?.startsWith("#/$defs/") ? reference.slice(8) : "";
  const generated = typeNames.get(name);
  if (!generated) throw new Error(`unknown generated reference ${String(reference)}`);
  return { kind: "ref", name: generated };
};
const isNull = (node) => node?.type === "null" || node?.const === null;
const unionNodes = (node) => node?.oneOf ?? node?.anyOf ?? null;
const declarations = new Map();

const typeFor = (node, context) => {
  if (node === true || (node && typeof node === "object" && Object.keys(node).length === 0)) {
    return { kind: "json" };
  }
  if (node === false) return { kind: "never" };
  if (!node || typeof node !== "object" || Array.isArray(node)) throw new Error(`invalid type at ${context}`);
  if (node.$ref) return refType(node.$ref);
  if (Object.hasOwn(node, "const")) {
    if (node.const === null) return { kind: "null" };
    if (typeof node.const === "boolean") return { kind: "boolean" };
    if (typeof node.const === "number") return { kind: "integer", bounded: true };
    return { kind: "string" };
  }
  if (Array.isArray(node.enum) && node.enum.every((value) => typeof value === "string")) {
    register(context, node);
    return { kind: "ref", name: context };
  }
  const union = unionNodes(node);
  if (union && !node.properties && node.type !== "object") {
    const nonNull = union.filter((entry) => !isNull(entry));
    if (nonNull.length === 1 && nonNull.length + 1 === union.length) {
      return { kind: "optional", inner: typeFor(nonNull[0], `${context}Value`) };
    }
    register(context, node);
    return { kind: "ref", name: context };
  }
  if (Array.isArray(node.allOf) && !node.type && !node.properties) {
    const referenced = node.allOf.find((entry) => entry?.$ref);
    return referenced ? refType(referenced.$ref) : { kind: "json" };
  }
  if (node.type === "string") return { kind: "string" };
  if (node.type === "integer") {
    return { kind: "integer", bounded: Number.isSafeInteger(node.minimum) && Number.isSafeInteger(node.maximum) };
  }
  if (node.type === "boolean") return { kind: "boolean" };
  if (node.type === "null") return { kind: "null" };
  if (node.type === "array") return { kind: "array", item: typeFor(node.items ?? true, `${context}Item`) };
  if (node.type === "object" || node.properties || Object.hasOwn(node, "additionalProperties")) {
    if (!node.properties && node.additionalProperties && typeof node.additionalProperties === "object") {
      return { kind: "map", value: typeFor(node.additionalProperties, `${context}Value`) };
    }
    register(context, node);
    return { kind: "ref", name: context };
  }
  throw new Error(`unsupported transport type shape at ${context}`);
};

function register(name, node) {
  if (declarations.has(name)) return;
  declarations.set(name, { kind: "pending", name });
  const definitionName = namesByType.get(name);
  if (definitionName === "jsonSafeValue") {
    declarations.set(name, { kind: "alias", name, type: { kind: "json" } });
    return;
  }
  if (Array.isArray(node.enum) && node.enum.every((value) => typeof value === "string")) {
    const variants = node.enum.map(enumVariant);
    if (new Set(variants).size !== variants.length) throw new Error(`enum collision in ${name}`);
    declarations.set(name, { kind: "enum", name, values: [...node.enum] });
    return;
  }
  if (Array.isArray(node.allOf) && !node.type && !node.properties) {
    const referenced = node.allOf.find((entry) => entry?.$ref);
    declarations.set(name, { kind: "alias", name, type: referenced ? refType(referenced.$ref) : { kind: "json" } });
    return;
  }
  const union = unionNodes(node);
  if (union && !node.properties && node.type !== "object") {
    const nonNull = union.filter((entry) => !isNull(entry));
    if (nonNull.length === 1 && nonNull.length + 1 === union.length) {
      declarations.set(name, { kind: "alias", name, type: { kind: "optional", inner: typeFor(nonNull[0], `${name}Value`) } });
      return;
    }
    const variants = nonNull.map((entry, index) => ({
      name: entry.$ref ? refType(entry.$ref).name : `${name}Variant${index + 1}`,
      type: typeFor(entry, `${name}Variant${index + 1}`),
    }));
    declarations.set(name, { kind: "union", name, variants });
    return;
  }
  if (node.type === "object" || node.properties || Object.hasOwn(node, "additionalProperties")) {
    const required = new Set(node.required ?? []);
    const fields = Object.entries(node.properties ?? {}).sort(([left], [right]) => compare(left, right))
      .map(([jsonName, property]) => ({
        jsonName,
        rustName: rustField(snake(jsonName)),
        cppName: cppField(snake(jsonName)),
        required: required.has(jsonName),
        type: typeFor(property, `${name}${pascal(jsonName)}`),
      }));
    for (const fieldKind of ["rustName", "cppName"]) {
      const names = fields.map((field) => field[fieldKind]);
      if (new Set(names).size !== names.length) throw new Error(`${fieldKind} collision in ${name}`);
    }
    const additional = node.additionalProperties === true ||
      (node.additionalProperties && typeof node.additionalProperties === "object")
      ? typeFor(node.additionalProperties === true ? true : node.additionalProperties, `${name}AdditionalValue`)
      : null;
    declarations.set(name, { kind: "object", name, fields, additional });
    return;
  }
  declarations.set(name, { kind: "alias", name, type: typeFor(node, `${name}Value`) });
}
for (const [name, node] of Object.entries(definitions)) register(typeNames.get(name), node);
if ([...declarations.values()].some((entry) => entry.kind === "pending")) {
  throw new Error("transport generator left unresolved declarations");
}

const referencedTypes = (type, result = new Set()) => {
  if (type.kind === "ref") result.add(type.name);
  else if (type.kind === "array") referencedTypes(type.item, result);
  else if (type.kind === "map") referencedTypes(type.value, result);
  else if (type.kind === "optional") referencedTypes(type.inner, result);
  return result;
};

const declarationDependencies = (declaration) => {
  const dependencies = new Set();
  if (declaration.kind === "alias") referencedTypes(declaration.type, dependencies);
  else if (declaration.kind === "union") {
    declaration.variants.forEach((variant) => referencedTypes(variant.type, dependencies));
  } else if (declaration.kind === "object") {
    declaration.fields.forEach((field) => referencedTypes(field.type, dependencies));
    if (declaration.additional) referencedTypes(declaration.additional, dependencies);
  }
  dependencies.delete(declaration.name);
  for (const dependency of dependencies) {
    if (!declarations.has(dependency)) {
      throw new Error(`generated declaration ${declaration.name} references unknown type ${dependency}`);
    }
  }
  return dependencies;
};

const orderedDeclarations = [];
const pendingDeclarations = new Map([...declarations.entries()].map(([name, declaration]) => [
  name,
  { declaration, dependencies: declarationDependencies(declaration) },
]));
while (pendingDeclarations.size > 0) {
  const ready = [...pendingDeclarations.entries()]
    .filter(([, entry]) => [...entry.dependencies].every((dependency) => !pendingDeclarations.has(dependency)))
    .sort(([left], [right]) => compare(left, right));
  if (ready.length === 0) {
    throw new Error(`generated declarations contain a by-value dependency cycle: ${[
      ...pendingDeclarations.keys(),
    ].sort(compare).join(", ")}`);
  }
  for (const [name, entry] of ready) {
    orderedDeclarations.push(entry.declaration);
    pendingDeclarations.delete(name);
  }
}

const rustType = (type) => {
  switch (type.kind) {
    case "ref": return type.name;
    case "string": return "String";
    case "integer": return type.bounded ? "i64" : "serde_json::Number";
    case "boolean": return "bool";
    case "null": return "()";
    case "json": return "serde_json::Value";
    case "never": return "serde_json::Value";
    case "array": return `Vec<${rustType(type.item)}>`;
    case "map": return `std::collections::BTreeMap<String, ${rustType(type.value)}>`;
    case "optional": return `Option<${rustType(type.inner)}>`;
    default: throw new Error(`unsupported Rust type ${type.kind}`);
  }
};

const renderRust = () => {
  const rustOption = (value) => value === null ? "None" : `Some(${JSON.stringify(value)})`;
  const out = [
    "// @generated by agent-runtime/aap-schema/scripts/generate-transport-types.mjs; do not edit.",
    "use serde::{Deserialize, Serialize};",
    "use serde_json::Value;",
    "use std::collections::BTreeMap;",
    "use std::fmt;",
    "use std::sync::OnceLock;",
    "",
    `pub const TRANSPORT_SCHEMA_ID: &str = ${JSON.stringify(expectedSchemaId)};`,
    `pub const TRANSPORT_SCHEMA_SHA256: &str = ${JSON.stringify(schemaSha256)};`,
    `pub const TRANSPORT_METHOD_REGISTRY_SHA256: &str = ${JSON.stringify(methodRegistrySha256)};`,
    `pub const TRANSPORT_SCHEMA_JSON: &str = ${JSON.stringify(JSON.stringify(schema))};`,
    "",
  ];
  for (const declaration of orderedDeclarations) {
    if (declaration.kind === "alias") {
      out.push(`pub type ${declaration.name} = ${rustType(declaration.type)};`, "");
    } else if (declaration.kind === "enum") {
      out.push("#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]", `pub enum ${declaration.name} {`);
      for (const value of declaration.values) {
        out.push(`    #[serde(rename = ${JSON.stringify(value)})]`, `    ${enumVariant(value)},`);
      }
      out.push("}", "");
    } else if (declaration.kind === "union") {
      out.push("#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]", "#[serde(untagged)]", `pub enum ${declaration.name} {`);
      declaration.variants.forEach((variant) => out.push(`    ${enumVariant(variant.name)}(${rustType(variant.type)}),`));
      out.push("}", "");
    } else if (declaration.kind === "object") {
      out.push("#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]", `pub struct ${declaration.name} {`);
      for (const field of declaration.fields) {
        if (field.rustName !== field.jsonName) out.push(`    #[serde(rename = ${JSON.stringify(field.jsonName)})]`);
        if (!field.required) out.push("    #[serde(default, skip_serializing_if = \"Option::is_none\")]");
        out.push(`    pub ${field.rustName}: ${field.required ? rustType(field.type) : `Option<${rustType(field.type)}>`},`);
      }
      if (declaration.additional) {
        out.push("    #[serde(flatten)]", `    pub additional: std::collections::BTreeMap<String, ${rustType(declaration.additional)}>,`);
      }
      out.push("}", "");
    }
  }
  out.push(
    "#[derive(Clone, Debug, PartialEq)]",
    "pub struct TransportMessage(pub Value);",
    "",
    "#[derive(Clone, Copy, Debug, PartialEq, Eq)]",
    "pub enum TransportMethodKind {",
    "    Request,",
    "    Notification,",
    "}",
    "",
    "#[derive(Clone, Copy, Debug, PartialEq, Eq)]",
    "pub struct TransportMethodMetadata {",
    "    pub method: &'static str,",
    "    pub kind: TransportMethodKind,",
    "    pub params_definition: Option<&'static str>,",
    "    pub request_definition: Option<&'static str>,",
    "    pub success_response_definition: Option<&'static str>,",
    "    pub success_result_definition: Option<&'static str>,",
    "    pub error_response_definitions: &'static [&'static str],",
    "    pub typed_error_stage: Option<&'static str>,",
    "    pub notification_definition: Option<&'static str>,",
    "}",
    "",
    "#[derive(Clone, Copy, Debug, PartialEq, Eq)]",
    "pub struct TransportTypedErrorMetadata {",
    "    pub method: &'static str,",
    "    pub schema_version: &'static str,",
    "    pub response_definition: &'static str,",
    "}",
    "",
    "pub const TRANSPORT_METHODS: &[TransportMethodMetadata] = &[",
    ...methodRegistry.methods.map((entry) => [
      "    TransportMethodMetadata {",
      `        method: ${JSON.stringify(entry.method)},`,
      `        kind: TransportMethodKind::${entry.kind === "request" ? "Request" : "Notification"},`,
      `        params_definition: ${rustOption(entry.params_definition)},`,
      `        request_definition: ${rustOption(entry.request_definition)},`,
      `        success_response_definition: ${rustOption(entry.success_response_definition)},`,
      `        success_result_definition: ${rustOption(entry.success_result_definition)},`,
      `        error_response_definitions: &[${entry.error_response_definitions.map(JSON.stringify).join(", ")}],`,
      `        typed_error_stage: ${rustOption(entry.typed_error_stage)},`,
      `        notification_definition: ${rustOption(entry.notification_definition)},`,
      "    },",
    ].join("\n")),
    "];",
    "",
    "pub const TRANSPORT_TYPED_ERRORS: &[TransportTypedErrorMetadata] = &[",
    ...typedErrorMetadata.map((entry) => [
      "    TransportTypedErrorMetadata {",
      `        method: ${JSON.stringify(entry.method)},`,
      `        schema_version: ${JSON.stringify(entry.schema_version)},`,
      `        response_definition: ${JSON.stringify(entry.response_definition)},`,
      "    },",
    ].join("\n")),
    "];",
    "",
    "pub fn transport_method_metadata(method: &str) -> Option<&'static TransportMethodMetadata> {",
    "    TRANSPORT_METHODS",
    "        .binary_search_by(|metadata| metadata.method.cmp(method))",
    "        .ok()",
    "        .map(|index| &TRANSPORT_METHODS[index])",
    "}",
    "",
    "pub fn transport_typed_error_metadata(method: &str, schema_version: &str) -> Option<&'static TransportTypedErrorMetadata> {",
    "    TRANSPORT_TYPED_ERRORS",
    "        .binary_search_by(|metadata| (metadata.method, metadata.schema_version).cmp(&(method, schema_version)))",
    "        .ok()",
    "        .map(|index| &TRANSPORT_TYPED_ERRORS[index])",
    "}",
    "",
    "fn transport_typed_error_schema_version_known(schema_version: &str) -> bool {",
    `    const SCHEMA_VERSIONS: [&str; ${typedErrorSchemaVersions.length}] = [${typedErrorSchemaVersions.map(JSON.stringify).join(", ")}];`,
    "    SCHEMA_VERSIONS.binary_search(&schema_version).is_ok()",
    "}",
    "",
    "#[derive(Clone, Debug, PartialEq)]",
    "#[non_exhaustive]",
    "pub enum TransportRequestOrNotification {",
    "    Known { message: TransportMessage, metadata: &'static TransportMethodMetadata },",
    "    UnknownRequest(TransportMessage),",
    "    UnknownNotification(TransportMessage),",
    "}",
    "",
    "#[derive(Clone, Debug, PartialEq)]",
    "#[non_exhaustive]",
    "pub enum TransportResponse {",
    "    KnownSuccess { message: TransportMessage, metadata: &'static TransportMethodMetadata },",
    "    KnownTypedError { message: TransportMessage, metadata: &'static TransportTypedErrorMetadata },",
    "    GenericError(TransportMessage),",
    "    UnknownMethod(TransportMessage),",
    "    Unmatched(TransportMessage),",
    "}",
    "",
    "#[derive(Clone, Copy, Debug, PartialEq, Eq)]",
    "pub struct TransportPendingRequest<'a> {",
    "    pub id: &'a str,",
    "    pub method: &'a str,",
    "    pub typed_error_request_identity: Option<&'a str>,",
    "}",
    "",
    "#[derive(Clone, Debug, PartialEq, Eq)]",
    "#[non_exhaustive]",
    "pub enum TransportDispatchError {",
    "    Parse(crate::transport_json::TransportJsonError),",
    "    InvalidEnvelope,",
    "    InvalidKnownMessage,",
    "    ValidatorUnavailable,",
    "}",
    "",
    "impl fmt::Display for TransportDispatchError {",
    "    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {",
    "        match self {",
    "            Self::Parse(error) => error.fmt(formatter),",
    "            Self::InvalidEnvelope => formatter.write_str(\"invalid generic transport envelope\"),",
    "            Self::InvalidKnownMessage => formatter.write_str(\"invalid known transport message\"),",
    "            Self::ValidatorUnavailable => {",
    "                formatter.write_str(\"generated transport dispatcher is unavailable\")",
    "            }",
    "        }",
    "    }",
    "}",
    "",
    "impl std::error::Error for TransportDispatchError {",
    "    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {",
    "        match self {",
    "            Self::Parse(error) => Some(error),",
    "            _ => None,",
    "        }",
    "    }",
    "}",
    "",
    "#[derive(Clone, Copy, Debug, PartialEq, Eq)]",
    "#[non_exhaustive]",
    "pub enum TransportSchemaError {",
    "    UnknownDefinition,",
    "    InvalidValue,",
    "    ValidatorUnavailable,",
    "}",
    "",
    "impl fmt::Display for TransportSchemaError {",
    "    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {",
    "        formatter.write_str(match self {",
    "            Self::UnknownDefinition => \"unknown generated transport schema definition\",",
    "            Self::InvalidValue => \"value does not match generated transport schema\",",
    "            Self::ValidatorUnavailable => \"generated transport schema validator is unavailable\",",
    "        })",
    "    }",
    "}",
    "",
    "impl std::error::Error for TransportSchemaError {}",
    "",
    "#[derive(Clone, Debug, PartialEq, Eq)]",
    "#[non_exhaustive]",
    "pub enum TransportDecodeError {",
    "    Parse(crate::transport_json::TransportJsonError),",
    "    Schema(TransportSchemaError),",
    "}",
    "",
    "impl fmt::Display for TransportDecodeError {",
    "    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {",
    "        match self {",
    "            Self::Parse(error) => error.fmt(formatter),",
    "            Self::Schema(error) => error.fmt(formatter),",
    "        }",
    "    }",
    "}",
    "",
    "impl std::error::Error for TransportDecodeError {",
    "    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {",
    "        match self {",
    "            Self::Parse(error) => Some(error),",
    "            Self::Schema(error) => Some(error),",
    "        }",
    "    }",
    "}",
    "",
    "impl From<crate::transport_json::TransportJsonError> for TransportDecodeError {",
    "    fn from(error: crate::transport_json::TransportJsonError) -> Self {",
    "        Self::Parse(error)",
    "    }",
    "}",
    "",
    "impl From<TransportSchemaError> for TransportDecodeError {",
    "    fn from(error: TransportSchemaError) -> Self {",
    "        Self::Schema(error)",
    "    }",
    "}",
    "",
    "#[rustfmt::skip]",
    `const TRANSPORT_DEFINITION_NAMES: [&str; ${Object.keys(definitions).length}] = [${Object.keys(definitions).sort(compare).map(JSON.stringify).join(", ")}];`,
    "#[rustfmt::skip]",
    "static TRANSPORT_SCHEMA: OnceLock<Result<Value, TransportSchemaError>> = OnceLock::new();",
    "#[rustfmt::skip]",
    "static TRANSPORT_ROOT_VALIDATOR: OnceLock<Result<jsonschema::Validator, TransportSchemaError>> = OnceLock::new();",
    "#[rustfmt::skip]",
    "static TRANSPORT_GENERIC_ENVELOPE_VALIDATOR: OnceLock<Result<jsonschema::Validator, TransportSchemaError>> = OnceLock::new();",
    "#[rustfmt::skip]",
    "type TransportDefinitionValidator = OnceLock<Result<jsonschema::Validator, TransportSchemaError>>;",
    "#[rustfmt::skip]",
    "static TRANSPORT_DEFINITION_VALIDATORS: OnceLock<BTreeMap<&'static str, TransportDefinitionValidator>> = OnceLock::new();",
    "",
    "#[cfg(test)]",
    "#[rustfmt::skip]",
    "static TRANSPORT_VALIDATOR_COMPILE_COUNTS: OnceLock<std::sync::Mutex<BTreeMap<&'static str, usize>>> = OnceLock::new();",
    "",
    "fn transport_schema() -> Result<&'static Value, TransportSchemaError> {",
    "    TRANSPORT_SCHEMA",
    "        .get_or_init(|| serde_json::from_str(TRANSPORT_SCHEMA_JSON).map_err(|_| TransportSchemaError::ValidatorUnavailable))",
    "        .as_ref()",
    "        .map_err(|error| *error)",
    "}",
    "",
    "fn record_validator_compile(target: &'static str) {",
    "    #[cfg(test)]",
    "    {",
    "        let counts = TRANSPORT_VALIDATOR_COMPILE_COUNTS.get_or_init(Default::default);",
    "        let mut counts = counts.lock().expect(\"validator compile count mutex\");",
    "        *counts.entry(target).or_default() += 1;",
    "    }",
    "    #[cfg(not(test))]",
    "    let _ = target;",
    "}",
    "",
    "fn compile_definition_validator(name: &'static str) -> Result<jsonschema::Validator, TransportSchemaError> {",
    "    record_validator_compile(name);",
    "    let root = transport_schema()?;",
    "    let rule = root",
    "        .get(\"$defs\")",
    "        .and_then(Value::as_object)",
    "        .and_then(|defs| defs.get(name))",
    "        .cloned()",
    "        .ok_or(TransportSchemaError::ValidatorUnavailable)?;",
    "    let document = serde_json::json!({",
    "        \"$schema\": root.get(\"$schema\").cloned().unwrap_or(Value::String(\"https://json-schema.org/draft/2020-12/schema\".to_owned())),",
    "        \"$defs\": root.get(\"$defs\").cloned().ok_or(TransportSchemaError::ValidatorUnavailable)?,",
    "        \"allOf\": [rule],",
    "    });",
    "    jsonschema::validator_for(&document).map_err(|_| TransportSchemaError::ValidatorUnavailable)",
    "}",
    "",
    "fn root_validator() -> Result<&'static jsonschema::Validator, TransportSchemaError> {",
    "    TRANSPORT_ROOT_VALIDATOR",
    "        .get_or_init(|| {",
    "            record_validator_compile(\"$root\");",
    "            jsonschema::validator_for(transport_schema()?).map_err(|_| TransportSchemaError::ValidatorUnavailable)",
    "        })",
    "        .as_ref()",
    "        .map_err(|error| *error)",
    "}",
    "",
    "fn generic_envelope_validator() -> Result<&'static jsonschema::Validator, TransportSchemaError> {",
    "    TRANSPORT_GENERIC_ENVELOPE_VALIDATOR",
    "        .get_or_init(|| {",
    "            record_validator_compile(\"$generic-envelope\");",
    "            let mut document = transport_schema()?",
    "                .clone()",
    "                .as_object()",
    "                .cloned()",
    "                .ok_or(TransportSchemaError::ValidatorUnavailable)?;",
    "            document.remove(\"allOf\")",
    "                .ok_or(TransportSchemaError::ValidatorUnavailable)?;",
    "            jsonschema::validator_for(&Value::Object(document))",
    "                .map_err(|_| TransportSchemaError::ValidatorUnavailable)",
    "        })",
    "        .as_ref()",
    "        .map_err(|error| *error)",
    "}",
    "",
    "fn definition_validator(name: &str) -> Result<&'static jsonschema::Validator, TransportSchemaError> {",
    "    let validators = TRANSPORT_DEFINITION_VALIDATORS.get_or_init(|| {",
    "        TRANSPORT_DEFINITION_NAMES",
    "            .iter()",
    "            .copied()",
    "            .map(|name| (name, OnceLock::new()))",
    "            .collect()",
    "    });",
    "    let (name, validator) = validators",
    "        .get_key_value(name)",
    "        .ok_or(TransportSchemaError::UnknownDefinition)?;",
    "    validator",
    "        .get_or_init(|| compile_definition_validator(name))",
    "        .as_ref()",
    "        .map_err(|error| *error)",
    "}",
    "",
    "pub fn validate_transport_definition(name: &str, value: &Value) -> Result<(), TransportSchemaError> {",
    "    if definition_validator(name)?.is_valid(value) { Ok(()) } else { Err(TransportSchemaError::InvalidValue) }",
    "}",
    "",
    "pub fn validate_transport_message(value: &Value) -> Result<(), TransportSchemaError> {",
    "    if root_validator()?.is_valid(value) { Ok(()) } else { Err(TransportSchemaError::InvalidValue) }",
    "}",
    "",
    "pub(crate) fn validate_transport_generic_envelope(value: &Value) -> Result<(), TransportSchemaError> {",
    "    if generic_envelope_validator()?.is_valid(value) { Ok(()) } else { Err(TransportSchemaError::InvalidValue) }",
    "}",
    "",
    "pub fn decode_transport_definition_raw(name: &str, bytes: &[u8]) -> Result<Value, TransportDecodeError> {",
    "    let value = crate::transport_json::parse_transport_json(bytes)?;",
    "    validate_transport_definition(name, &value).map_err(TransportDecodeError::Schema)?;",
    "    Ok(value)",
    "}",
    "",
    "pub fn decode_transport_message_raw(bytes: &[u8]) -> Result<TransportMessage, TransportDecodeError> {",
    "    let value = crate::transport_json::parse_transport_json(bytes)?;",
    "    validate_transport_message(&value).map_err(TransportDecodeError::Schema)?;",
    "    Ok(TransportMessage(value))",
    "}",
    "",
    "pub(crate) fn decode_transport_generic_envelope_raw(bytes: &[u8]) -> Result<TransportMessage, TransportDecodeError> {",
    "    let value = crate::transport_json::parse_transport_json(bytes)?;",
    "    validate_transport_generic_envelope(&value).map_err(TransportDecodeError::Schema)?;",
    "    Ok(TransportMessage(value))",
    "}",
    "",
    "fn transport_error_schema_version(value: &Value) -> Option<&str> {",
    "    value.as_object()?",
    "        .get(\"error\")?.as_object()?",
    "        .get(\"data\")?.as_object()?",
    "        .get(\"schema_version\")?.as_str()",
    "}",
    "",
    "fn transport_dispatch_schema_error(error: TransportSchemaError, invalid: TransportDispatchError) -> TransportDispatchError {",
    "    match error {",
    "        TransportSchemaError::InvalidValue => invalid,",
    "        TransportSchemaError::UnknownDefinition | TransportSchemaError::ValidatorUnavailable =>",
    "            TransportDispatchError::ValidatorUnavailable,",
    "    }",
    "}",
    "",
    "fn transport_dispatch_decode_error(error: TransportDecodeError) -> TransportDispatchError {",
    "    match error {",
    "        TransportDecodeError::Parse(error) => TransportDispatchError::Parse(error),",
    "        TransportDecodeError::Schema(error) =>",
    "            transport_dispatch_schema_error(error, TransportDispatchError::InvalidEnvelope),",
    "    }",
    "}",
    "",
    "fn validate_typed_error_correlation(metadata: &TransportMethodMetadata, pending: TransportPendingRequest<'_>, value: &Value) -> Result<(), TransportDispatchError> {",
    "    if metadata.typed_error_stage.is_none() && pending.typed_error_request_identity.is_none() { return Ok(()); }",
    "    let data = value.as_object()",
    "        .and_then(|object| object.get(\"error\"))",
    "        .and_then(Value::as_object)",
    "        .and_then(|error| error.get(\"data\"))",
    "        .and_then(Value::as_object)",
    "        .ok_or(TransportDispatchError::InvalidKnownMessage)?;",
    "    let expected_stage = metadata.typed_error_stage.ok_or(TransportDispatchError::InvalidKnownMessage)?;",
    "    let expected_identity = pending.typed_error_request_identity.ok_or(TransportDispatchError::InvalidKnownMessage)?;",
    "    if data.get(\"stage\").and_then(Value::as_str) != Some(expected_stage) ||",
    "        data.get(\"request_identity\").and_then(Value::as_str) != Some(expected_identity) {",
    "        return Err(TransportDispatchError::InvalidKnownMessage);",
    "    }",
    "    Ok(())",
    "}",
    "",
    "pub fn decode_transport_request_or_notification_raw(bytes: &[u8]) -> Result<TransportRequestOrNotification, TransportDispatchError> {",
    "    let message = decode_transport_generic_envelope_raw(bytes).map_err(transport_dispatch_decode_error)?;",
    "    let object = message.0.as_object().ok_or(TransportDispatchError::InvalidEnvelope)?;",
    "    let method = object.get(\"method\").and_then(Value::as_str)",
    "        .ok_or(TransportDispatchError::InvalidEnvelope)?;",
    "    let is_request = object.contains_key(\"id\");",
    "    match transport_method_metadata(method) {",
    "        Some(metadata) => {",
    "            let expected_request = metadata.kind == TransportMethodKind::Request;",
    "            if expected_request != is_request {",
    "                return Err(TransportDispatchError::InvalidKnownMessage);",
    "            }",
    "            let definition = if is_request { metadata.request_definition } else { metadata.notification_definition };",
    "            match definition {",
    "                Some(definition) => validate_transport_definition(definition, &message.0)",
    "                    .map_err(|error| transport_dispatch_schema_error(error, TransportDispatchError::InvalidKnownMessage))?,",
    "                None => validate_transport_message(&message.0)",
    "                    .map_err(|error| transport_dispatch_schema_error(error, TransportDispatchError::InvalidKnownMessage))?,",
    "            }",
    "            Ok(TransportRequestOrNotification::Known { message, metadata })",
    "        }",
    "        None if is_request => Ok(TransportRequestOrNotification::UnknownRequest(message)),",
    "        None => Ok(TransportRequestOrNotification::UnknownNotification(message)),",
    "    }",
    "}",
    "",
    "pub fn decode_transport_response_raw(pending: Option<TransportPendingRequest<'_>>, bytes: &[u8]) -> Result<TransportResponse, TransportDispatchError> {",
    "    let message = decode_transport_generic_envelope_raw(bytes).map_err(transport_dispatch_decode_error)?;",
    "    let object = message.0.as_object().ok_or(TransportDispatchError::InvalidEnvelope)?;",
    "    if object.contains_key(\"method\") {",
    "        return Err(TransportDispatchError::InvalidEnvelope);",
    "    }",
    "    let response_id = object.get(\"id\").and_then(Value::as_str);",
    "    if pending.is_some_and(|context| response_id != Some(context.id)) {",
    "        if let Some(schema_version) = transport_error_schema_version(&message.0) {",
    "            if transport_typed_error_schema_version_known(schema_version) {",
    "                let error_metadata = TRANSPORT_TYPED_ERRORS.iter()",
    "                    .find(|metadata| metadata.schema_version == schema_version)",
    "                    .ok_or(TransportDispatchError::ValidatorUnavailable)?;",
    "                validate_transport_definition(error_metadata.response_definition, &message.0)",
    "                    .map_err(|error| transport_dispatch_schema_error(error, TransportDispatchError::InvalidKnownMessage))?;",
    "            }",
    "        }",
    "        return Ok(TransportResponse::Unmatched(message));",
    "    }",
    "    let typed_schema_version = transport_error_schema_version(&message.0);",
    "    match pending.and_then(|context| transport_method_metadata(context.method)) {",
    "        Some(metadata) if metadata.kind == TransportMethodKind::Request => {",
    "            if object.contains_key(\"result\") {",
    "                let definition = metadata.success_response_definition",
    "                    .ok_or(TransportDispatchError::ValidatorUnavailable)?;",
    "                validate_transport_definition(definition, &message.0)",
    "                    .map_err(|error| transport_dispatch_schema_error(error, TransportDispatchError::InvalidKnownMessage))?;",
    "                return Ok(TransportResponse::KnownSuccess { message, metadata });",
    "            }",
    "            if let Some(schema_version) = typed_schema_version {",
    "                if let Some(error_metadata) = transport_typed_error_metadata(metadata.method, schema_version) {",
    "                    validate_transport_definition(error_metadata.response_definition, &message.0)",
    "                        .map_err(|error| transport_dispatch_schema_error(error, TransportDispatchError::InvalidKnownMessage))?;",
    "                    validate_typed_error_correlation(metadata, pending.expect(\"matched pending request\"), &message.0)?;",
    "                    return Ok(TransportResponse::KnownTypedError { message, metadata: error_metadata });",
    "                }",
    "                if transport_typed_error_schema_version_known(schema_version) {",
    "                    return Err(TransportDispatchError::InvalidKnownMessage);",
    "                }",
    "            }",
    "            Ok(TransportResponse::GenericError(message))",
    "        }",
    "        Some(_) => Err(TransportDispatchError::InvalidKnownMessage),",
    "        None if pending.is_some() => {",
    "            if typed_schema_version.is_some_and(transport_typed_error_schema_version_known) {",
    "                return Err(TransportDispatchError::InvalidKnownMessage);",
    "            }",
    "            Ok(TransportResponse::UnknownMethod(message))",
    "        }",
    "        None => {",
    "            if let Some(schema_version) = typed_schema_version {",
    "                if transport_typed_error_schema_version_known(schema_version) {",
    "                    let error_metadata = TRANSPORT_TYPED_ERRORS.iter()",
    "                        .find(|metadata| metadata.schema_version == schema_version)",
    "                        .ok_or(TransportDispatchError::ValidatorUnavailable)?;",
    "                    validate_transport_definition(error_metadata.response_definition, &message.0)",
    "                        .map_err(|error| transport_dispatch_schema_error(error, TransportDispatchError::InvalidKnownMessage))?;",
    "                }",
    "            }",
    "            Ok(TransportResponse::Unmatched(message))",
    "        }",
    "    }",
    "}",
    "",
    "#[cfg(test)]",
    "pub(crate) fn transport_dispatch_test_map_schema_error(error: TransportSchemaError) -> TransportDispatchError {",
    "    transport_dispatch_schema_error(error, TransportDispatchError::InvalidKnownMessage)",
    "}",
    "",
    "#[cfg(test)]",
    "pub(crate) fn transport_validator_compile_count(target: &str) -> usize {",
    "    TRANSPORT_VALIDATOR_COMPILE_COUNTS",
    "        .get()",
    "        .and_then(|counts| {",
    "            counts",
    "                .lock()",
    "                .ok()",
    "                .and_then(|counts| counts.get(target).copied())",
    "        })",
    "        .unwrap_or(0)",
    "}",
    "",
  );
  const rendered = out.join("\n").replace(
    /^(pub(?:\(crate\))? (?:const|type|enum|struct|fn)|fn )/gm,
    "#[rustfmt::skip]\n$&",
  );
  return `${rendered.replace(/\n+$/u, "")}\n`;
};

const tsType = (type) => {
  switch (type.kind) {
    case "ref": return type.name;
    case "string": return "string";
    case "integer": return type.bounded ? "number" : "TransportJsonNumber";
    case "boolean": return "boolean";
    case "null": return "null";
    case "json": return "TransportJsonValue";
    case "never": return "never";
    case "array": return `Array<${tsType(type.item)}>`;
    case "map": return `{ [key: string]: ${tsType(type.value)} }`;
    case "optional": return `${tsType(type.inner)} | null`;
    default: throw new Error(`unsupported TypeScript type ${type.kind}`);
  }
};

const renderTypeScriptDeclarations = () => {
  const out = [
    "// @generated by agent-runtime/aap-schema/scripts/generate-transport-types.mjs; do not edit.",
    "declare const transportJsonNumberBrand: unique symbol;",
    "export interface TransportJsonNumber { readonly lexical: string; readonly canonical: string; readonly integer: boolean; readonly [transportJsonNumberBrand]: never; }",
    "export type TransportJsonValue = null | boolean | string | TransportJsonNumber | Array<TransportJsonValue> | { [key: string]: TransportJsonValue };",
    "",
  ];
  for (const declaration of orderedDeclarations) {
    if (declaration.kind === "alias") out.push(`export type ${declaration.name} = ${tsType(declaration.type)};`, "");
    else if (declaration.kind === "enum") out.push(`export type ${declaration.name} = ${declaration.values.map(JSON.stringify).join(" | ")};`, "");
    else if (declaration.kind === "union") out.push(`export type ${declaration.name} = ${declaration.variants.map((variant) => tsType(variant.type)).join(" | ")};`, "");
    else if (declaration.kind === "object") {
      out.push(`export interface ${declaration.name} {`);
      declaration.fields.forEach((field) => out.push(`  ${JSON.stringify(field.jsonName)}${field.required ? "" : "?"}: ${tsType(field.type)};`));
      if (declaration.additional) out.push(`  [key: string]: ${tsType(declaration.additional)};`);
      out.push("}", "");
    }
  }
  out.push(
    "export interface TransportMessage { readonly value: TransportJsonValue; }",
    "export function validateTransportDefinitionRaw(name: string, raw: string): TransportJsonValue;",
    "export function validateTransportMessageRaw(raw: string): TransportMessage;",
    "export function isTransportJsonNumber(value: unknown): value is TransportJsonNumber;",
    "export function canonicalTransportJson(value: TransportJsonValue): string;",
    "export const TRANSPORT_SCHEMA_ID: string;",
    "export const TRANSPORT_SCHEMA_SHA256: string;",
    "",
  );
  return `${out.join("\n").replace(/\n+$/u, "")}\n`;
};

const renderTypeScriptRuntime = () => `// @generated by agent-runtime/aap-schema/scripts/generate-transport-types.mjs; do not edit.
import { canonicalTransportJson, createTransportOracle, isTransportJsonNumber } from "../../scripts/transport-schema-oracle.mjs";

export const TRANSPORT_SCHEMA_ID = ${JSON.stringify(expectedSchemaId)};
export const TRANSPORT_SCHEMA_SHA256 = ${JSON.stringify(schemaSha256)};
const schema = ${JSON.stringify(schema)};
const oracle = createTransportOracle(schema);

export const validateTransportDefinitionRaw = (name, raw) => oracle.validateDefinitionRaw(name, raw);
export const validateTransportMessageRaw = (raw) => ({ value: oracle.validateRootRaw(raw) });
export { canonicalTransportJson, isTransportJsonNumber };
`;

const cppType = (type) => {
  switch (type.kind) {
    case "ref": return type.name;
    case "string": return "QString";
    case "integer": return type.bounded ? "qint64" : "TransportJsonNumber";
    case "boolean": return "bool";
    case "null": return "std::monostate";
    case "json": return "TransportJsonValue";
    case "never": return "TransportJsonValue";
    case "array": return `QList<${cppType(type.item)}>`;
    case "map": return `QMap<QString, ${cppType(type.value)}>`;
    case "optional": return `std::optional<${cppType(type.inner)}>`;
    default: throw new Error(`unsupported C++ type ${type.kind}`);
  }
};

const renderCppHeader = () => {
  const out = [
    "// @generated by agent-runtime/aap-schema/scripts/generate-transport-types.mjs; do not edit.",
    "#pragma once",
    "#include <QByteArray>",
    "#include <QList>",
    "#include <QMap>",
    "#include <QString>",
    "#include <QVariant>",
    "#include <optional>",
    "#include <variant>",
    "",
    "namespace aegisy::aap::transport_generated {",
    "struct TransportJsonNumber { QString lexical; QString canonical; bool integer = false; };",
    "struct TransportJsonValue {",
    "    using Array = QList<TransportJsonValue>;",
    "    using Object = QMap<QString, TransportJsonValue>;",
    "    std::variant<std::monostate, bool, QString, TransportJsonNumber, Array, Object> value;",
    "};",
    "",
  ];
  for (const declaration of orderedDeclarations) {
    if (declaration.kind === "object" || declaration.kind === "union") out.push(`struct ${declaration.name};`);
  }
  out.push("");
  for (const declaration of orderedDeclarations) {
    if (declaration.kind === "alias") out.push(`using ${declaration.name} = ${cppType(declaration.type)};`, "");
    else if (declaration.kind === "enum") {
      out.push(`enum class ${declaration.name} { ${declaration.values.map(enumVariant).join(", ")} };`, "");
    } else if (declaration.kind === "union") {
      out.push(`struct ${declaration.name} { std::variant<${declaration.variants.map((variant) => cppType(variant.type)).join(", ")}> value; };`, "");
    } else if (declaration.kind === "object") {
      out.push(`struct ${declaration.name} {`);
      declaration.fields.forEach((field) => out.push(`    ${field.required ? cppType(field.type) : `std::optional<${cppType(field.type)}>`} ${field.cppName};`));
      if (declaration.additional) out.push(`    QMap<QString, ${cppType(declaration.additional)}> additional;`);
      out.push("};", "");
    }
  }
  out.push(
    "struct TransportMessage { TransportJsonValue value; };",
    "bool validateTransportDefinitionRaw(const QString &name, const QByteArray &raw, TransportJsonValue *output = nullptr, QString *error = nullptr);",
    "bool validateTransportMessageRaw(const QByteArray &raw, TransportMessage *output = nullptr, QString *error = nullptr);",
    "QByteArray canonicalTransportJson(const TransportJsonValue &value);",
    "",
    "} // namespace aegisy::aap::transport_generated",
    "",
  );
  return `${out.join("\n").replace(/\n+$/u, "")}\n`;
};

const renderCppSource = () => `// @generated by agent-runtime/aap-schema/scripts/generate-transport-types.mjs; do not edit.
#include "aap_transport_types_generated.h"
#include "aap_transport_runtime.h"

#include <memory>
#include <utility>

namespace aegisy::aap::transport_generated {
namespace {
const transport_runtime::TransportSchemaRuntime *transportRuntime(QString *error)
{
    static QString initializationError;
    static const std::unique_ptr<transport_runtime::TransportSchemaRuntime> runtime =
        transport_runtime::TransportSchemaRuntime::fromRawSchema(
            QByteArrayLiteral(R"AAPSCHEMA(${JSON.stringify(schema)})AAPSCHEMA"),
            &initializationError);
    if (!runtime && error) *error = initializationError;
    return runtime.get();
}
} // namespace

bool validateTransportDefinitionRaw(const QString &name, const QByteArray &raw,
                                    TransportJsonValue *output, QString *error)
{
    const auto *runtime = transportRuntime(error);
    return runtime && runtime->validateDefinitionRaw(name, raw, output, error);
}

bool validateTransportMessageRaw(const QByteArray &raw, TransportMessage *output,
                                 QString *error)
{
    const auto *runtime = transportRuntime(error);
    if (!runtime) return false;
    TransportJsonValue value;
    if (!runtime->validateRootRaw(raw, &value, error)) return false;
    if (output) output->value = std::move(value);
    return true;
}

QByteArray canonicalTransportJson(const TransportJsonValue &value)
{
    return transport_runtime::canonicalTransportJson(value);
}
} // namespace aegisy::aap::transport_generated
`;

const outputs = new Map([
  [resolve(packageRoot, "../crates/aegisy-aap/src/generated_transport.rs"), renderRust()],
  [resolve(packageRoot, "generated/typescript/transport_types.d.ts"), renderTypeScriptDeclarations()],
  [resolve(packageRoot, "generated/typescript/transport_types.mjs"), renderTypeScriptRuntime()],
  [resolve(packageRoot, "generated/cpp/aap_transport_types_generated.h"), renderCppHeader()],
  [resolve(packageRoot, "generated/cpp/aap_transport_types_generated.cpp"), renderCppSource()],
]);

let stale = false;
for (const [outputPath, content] of outputs) {
  if (checkOnly) {
    let existing = "";
    try { existing = readFileSync(outputPath, "utf8"); } catch { /* missing output is stale */ }
    if (existing !== content) {
      console.error(`generated output is stale: ${relative(packageRoot, outputPath).replaceAll("\\", "/")}`);
      stale = true;
    }
  } else {
    mkdirSync(dirname(outputPath), { recursive: true });
    writeFileSync(outputPath, content);
  }
}
if (stale) process.exitCode = 1;
