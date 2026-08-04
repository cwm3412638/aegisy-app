import { createHash } from "node:crypto";
import { readFileSync, mkdirSync, writeFileSync } from "node:fs";
import { dirname, relative, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
let schemaPath = resolve(packageRoot, "stable/v0.1/core.schema.json");
let fixtureMapPath = resolve(packageRoot, "fixtures/aap-core-domains.fixture-map.json");
let fixtureCatalogPath = resolve(packageRoot, "fixtures/aap-core-domains.json");
let checkOnly = false;
for (let index = 2; index < process.argv.length; index += 1) {
  const argument = process.argv[index];
  if (argument === "--check") {
    checkOnly = true;
  } else if (argument === "--schema" || argument === "--fixture-map" || argument === "--fixture-catalog") {
    const value = process.argv[index + 1];
    if (!value) throw new Error(`${argument} requires a path`);
    if (argument === "--schema") schemaPath = resolve(value);
    else if (argument === "--fixture-map") fixtureMapPath = resolve(value);
    else fixtureCatalogPath = resolve(value);
    index += 1;
  } else {
    throw new Error(`unsupported generator argument ${argument}`);
  }
}
const schemaBytes = readFileSync(schemaPath);
const schema = JSON.parse(schemaBytes);
const fixtureMap = JSON.parse(readFileSync(fixtureMapPath, "utf8"));
const fixtureCatalog = JSON.parse(readFileSync(fixtureCatalogPath, "utf8"));

const requireExactKeys = (value, required, optional, context) => {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    throw new Error(`${context} must be an object`);
  }
  const allowed = new Set([...required, ...optional]);
  for (const key of Object.keys(value)) {
    if (!allowed.has(key)) throw new Error(`${context} contains unknown field ${key}`);
  }
  for (const key of required) {
    if (!Object.hasOwn(value, key)) throw new Error(`${context} is missing ${key}`);
  }
};

requireExactKeys(
  fixtureMap,
  ["schema_version", "schema_id", "canonical_bytes", "canonical_sha256", "entries"],
  [],
  "fixture map",
);
if (!Array.isArray(fixtureMap.entries)) throw new Error("fixture map entries must be an array");
requireExactKeys(fixtureCatalog, [], Object.keys(fixtureCatalog), "fixture catalog");

const expectedSchemaId = "https://aegisy.cc/schemas/aap/stable/v0.1/core.schema.json";
if (schema.$id !== expectedSchemaId || fixtureMap.schema_id !== expectedSchemaId) {
  throw new Error("core schema and fixture map identity must match AAP stable 0.1");
}
if (fixtureMap.schema_version !== "aap-core-fixture-map/0.1") {
  throw new Error("unsupported core fixture map version");
}
if (!Number.isSafeInteger(fixtureMap.canonical_bytes) || fixtureMap.canonical_bytes < 0) {
  throw new Error("fixture map canonical_bytes must be a safe non-negative integer");
}
if (typeof fixtureMap.canonical_sha256 !== "string" || !/^[0-9a-f]{64}$/.test(fixtureMap.canonical_sha256)) {
  throw new Error("fixture map canonical_sha256 must be a lowercase SHA-256 digest");
}

const schemaSha256 = createHash("sha256").update(schemaBytes).digest("hex");
const definitions = schema.$defs;
if (!definitions || typeof definitions !== "object" || Array.isArray(definitions)) {
  throw new Error("core schema must define an object $defs library");
}

const supportedSchemaKeywords = new Set([
  "$comment", "$defs", "$id", "$ref", "$schema", "additionalProperties",
  "allOf", "anyOf", "const", "contains", "else", "enum", "if", "items",
  "maxContains", "maxItems", "maxLength", "maxProperties", "maximum",
  "minContains", "minItems", "minLength", "minProperties", "minimum", "not",
  "oneOf", "pattern", "properties", "propertyNames", "required", "then",
  "title", "type", "uniqueItems",
]);
const supportedSchemaTypes = new Set(["null", "boolean", "integer", "string", "array", "object"]);
const boundedCardinalityKeywords = [
  "minContains", "maxContains", "minItems", "maxItems", "minLength", "maxLength",
  "minProperties", "maxProperties",
];

const auditSchemaNode = (node, context, root = false) => {
  if (!node || typeof node !== "object" || Array.isArray(node)) {
    throw new Error(`${context} must be an object schema`);
  }
  for (const key of Object.keys(node)) {
    if (!supportedSchemaKeywords.has(key)) {
      throw new Error(`${context} uses unsupported schema keyword ${key}`);
    }
  }
  for (const keyword of ["$schema", "$id", "$defs"]) {
    if (!root && Object.hasOwn(node, keyword)) {
      throw new Error(`${context} uses unsupported nested ${keyword}`);
    }
  }
  if (Object.hasOwn(node, "$schema") && node.$schema !== "https://json-schema.org/draft/2020-12/schema") {
    throw new Error(`${context} uses an unsupported JSON Schema dialect`);
  }
  for (const keyword of ["$id", "$comment", "title"]) {
    if (Object.hasOwn(node, keyword) && typeof node[keyword] !== "string") {
      throw new Error(`${context}.${keyword} must be a string`);
    }
  }
  if (Object.hasOwn(node, "$ref")) {
    const prefix = "#/$defs/";
    if (typeof node.$ref !== "string" || !node.$ref.startsWith(prefix) ||
        !Object.hasOwn(definitions, node.$ref.slice(prefix.length))) {
      throw new Error(`${context} has an unsupported or unknown $ref`);
    }
    const semanticSiblings = Object.keys(node)
      .filter((key) => !["$ref", "$comment", "title"].includes(key));
    if (semanticSiblings.length) {
      throw new Error(`${context} combines $ref with unsupported semantic siblings`);
    }
  }
  if (Object.hasOwn(node, "type") &&
      (typeof node.type !== "string" || !supportedSchemaTypes.has(node.type))) {
    throw new Error(`${context}.type is unsupported`);
  }
  if (node.type === "integer" &&
      (!Number.isSafeInteger(node.minimum) || !Number.isSafeInteger(node.maximum))) {
    throw new Error(`${context} integer schemas must declare JSON-safe bounds`);
  }
  for (const keyword of ["minimum", "maximum"]) {
    if (Object.hasOwn(node, keyword) && !Number.isSafeInteger(node[keyword])) {
      throw new Error(`${context}.${keyword} must be a JSON-safe integer`);
    }
  }
  if (Number.isSafeInteger(node.minimum) && Number.isSafeInteger(node.maximum) && node.minimum > node.maximum) {
    throw new Error(`${context} has an inverted numeric range`);
  }
  for (const keyword of boundedCardinalityKeywords) {
    if (Object.hasOwn(node, keyword) &&
        (!Number.isSafeInteger(node[keyword]) || node[keyword] < 0 || node[keyword] > 2147483647)) {
      throw new Error(`${context}.${keyword} must be a non-negative 32-bit integer`);
    }
  }
  for (const [minimum, maximum] of [
    ["minContains", "maxContains"], ["minItems", "maxItems"],
    ["minLength", "maxLength"], ["minProperties", "maxProperties"],
  ]) {
    if (Number.isSafeInteger(node[minimum]) && Number.isSafeInteger(node[maximum]) &&
        node[minimum] > node[maximum]) {
      throw new Error(`${context} has an inverted ${minimum}/${maximum} range`);
    }
  }
  if ((Object.hasOwn(node, "minContains") || Object.hasOwn(node, "maxContains")) &&
      !Object.hasOwn(node, "contains")) {
    throw new Error(`${context} bounds contains without a contains schema`);
  }
  if ((Object.hasOwn(node, "then") || Object.hasOwn(node, "else")) && !Object.hasOwn(node, "if")) {
    throw new Error(`${context} uses then/else without if`);
  }
  if (Object.hasOwn(node, "pattern")) {
    if (typeof node.pattern !== "string") throw new Error(`${context}.pattern must be a string`);
    try { new RegExp(node.pattern, "u"); } catch (_) { throw new Error(`${context}.pattern is invalid`); }
  }
  if (Object.hasOwn(node, "uniqueItems") && typeof node.uniqueItems !== "boolean") {
    throw new Error(`${context}.uniqueItems must be boolean`);
  }
  if (Object.hasOwn(node, "required") &&
      (!Array.isArray(node.required) || node.required.some((key) => typeof key !== "string") ||
       new Set(node.required).size !== node.required.length)) {
    throw new Error(`${context}.required must contain unique strings`);
  }
  if (Object.hasOwn(node, "enum") && (!Array.isArray(node.enum) || node.enum.length === 0)) {
    throw new Error(`${context}.enum must be a non-empty array`);
  }
  if (Array.isArray(node.enum) &&
      new Set(node.enum.map((value) => JSON.stringify(value))).size !== node.enum.length) {
    throw new Error(`${context}.enum must contain unique values`);
  }

  for (const keyword of ["$defs", "properties"]) {
    if (!Object.hasOwn(node, keyword)) continue;
    const children = node[keyword];
    if (!children || typeof children !== "object" || Array.isArray(children)) {
      throw new Error(`${context}.${keyword} must be an object`);
    }
    for (const [name, child] of Object.entries(children)) {
      auditSchemaNode(child, `${context}.${keyword}.${name}`);
    }
  }
  for (const keyword of ["items", "contains", "propertyNames", "if", "then", "else", "not"]) {
    if (Object.hasOwn(node, keyword)) auditSchemaNode(node[keyword], `${context}.${keyword}`);
  }
  if (Object.hasOwn(node, "additionalProperties")) {
    const additional = node.additionalProperties;
    if (typeof additional !== "boolean") {
      auditSchemaNode(additional, `${context}.additionalProperties`);
    }
  }
  for (const keyword of ["oneOf", "anyOf", "allOf"]) {
    if (!Object.hasOwn(node, keyword)) continue;
    const children = node[keyword];
    if (!Array.isArray(children) || children.length === 0) {
      throw new Error(`${context}.${keyword} must be a non-empty array`);
    }
    children.forEach((child, index) => auditSchemaNode(child, `${context}.${keyword}[${index}]`));
  }
};

auditSchemaNode(schema, "core schema", true);

const cppSchema = structuredClone(schema);
const normalizeCppPatterns = (value) => {
  if (Array.isArray(value)) {
    value.forEach(normalizeCppPatterns);
    return;
  }
  if (!value || typeof value !== "object") return;
  for (const [key, child] of Object.entries(value)) {
    if (key === "pattern") value[key] = child.replace(/\\u([0-9A-Fa-f]{4})/g, "\\x{$1}");
    else normalizeCppPatterns(child);
  }
};
normalizeCppPatterns(cppSchema);
const declarations = new Map();
const declarationSources = new Map();

const compareOrdinal = (left, right) => left < right ? -1 : left > right ? 1 : 0;

const pascal = (value) => value
  .replace(/[^A-Za-z0-9]+(.)?/g, (_match, next) => next ? next.toUpperCase() : "")
  .replace(/^[a-z]/, (first) => first.toUpperCase());

const definitionTypeNames = new Map();
const definitionNamesByType = new Map();
for (const definitionName of Object.keys(definitions)) {
  const generatedName = pascal(definitionName);
  if (!/^[A-Za-z][A-Za-z0-9]*$/.test(generatedName)) {
    throw new Error(`definition ${definitionName} does not produce a valid generated type name`);
  }
  const previous = definitionNamesByType.get(generatedName);
  if (previous !== undefined) {
    throw new Error(
      `generated type name collision: definitions ${previous} and ${definitionName} both map to ${generatedName}`,
    );
  }
  definitionTypeNames.set(definitionName, generatedName);
  definitionNamesByType.set(generatedName, definitionName);
}

const snake = (value) => value
  .replace(/([a-z0-9])([A-Z])/g, "$1_$2")
  .replace(/[^A-Za-z0-9]+/g, "_")
  .replace(/^_+|_+$/g, "")
  .toLowerCase();

const rustReserved = new Set([
  "as", "async", "await", "break", "const", "continue", "crate", "dyn", "else",
  "enum", "extern", "false", "final", "fn", "for", "if", "impl", "in", "let",
  "loop", "macro", "match", "mod", "move", "override", "priv", "pub", "ref",
  "return", "self", "static", "struct", "super", "trait", "true", "try", "type",
  "typeof", "union", "unsafe", "unsized", "use", "virtual", "where", "while", "yield",
]);

const rustField = (value) => rustReserved.has(value) ? `${value}_value` : value;

const cppReserved = new Set([
  "alignas", "alignof", "and", "asm", "auto", "bitand", "bitor", "bool",
  "break", "case", "catch", "char", "class", "compl", "concept", "const",
  "constexpr", "continue", "co_await", "co_return", "co_yield", "decltype",
  "default", "delete", "do", "double", "dynamic_cast", "else", "enum",
  "explicit", "export", "extern", "false", "float", "for", "friend", "goto",
  "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept",
  "not", "nullptr", "operator", "or", "private", "protected", "public",
  "register", "reinterpret_cast", "requires", "return", "short", "signed",
  "sizeof", "static", "static_assert", "static_cast", "struct", "switch",
  "template", "this", "thread_local", "throw", "true", "try", "typedef",
  "typeid", "typename", "union", "unsigned", "using", "virtual", "void",
  "volatile", "wchar_t", "while", "xor",
]);

const cppField = (value) => cppReserved.has(value) ? `${value}_value` : value;

const fixtureKeys = new Set();
for (const [index, entry] of fixtureMap.entries.entries()) {
  requireExactKeys(entry, ["key", "definition"], ["array"], `fixture map entry ${index}`);
  if (typeof entry.key !== "string" || !/^[A-Za-z][A-Za-z0-9]*$/.test(entry.key)) {
    throw new Error(`fixture map entry ${index} has an invalid key`);
  }
  if (typeof entry.definition !== "string" || !/^[A-Za-z][A-Za-z0-9]*$/.test(entry.definition)) {
    throw new Error(`fixture map entry ${index} has an invalid definition`);
  }
  if (Object.hasOwn(entry, "array") && typeof entry.array !== "boolean") {
    throw new Error(`fixture map entry ${index} array must be boolean`);
  }
  if (fixtureKeys.has(entry.key)) throw new Error(`duplicate fixture map key ${entry.key}`);
  fixtureKeys.add(entry.key);
}

const catalogKeys = Object.keys(fixtureCatalog);
const missingFixtureKeys = [...fixtureKeys].filter((key) => !Object.hasOwn(fixtureCatalog, key));
const unexpectedFixtureKeys = catalogKeys.filter((key) => !fixtureKeys.has(key));
if (missingFixtureKeys.length || unexpectedFixtureKeys.length) {
  throw new Error(
    `fixture map/catalog keys differ: missing=${missingFixtureKeys.join(",")} unexpected=${unexpectedFixtureKeys.join(",")}`,
  );
}

const enumVariant = (value) => {
  const candidate = pascal(String(value));
  return /^[0-9]/.test(candidate) ? `Value${candidate}` : candidate || "Value";
};

const refName = (ref) => {
  const prefix = "#/$defs/";
  if (typeof ref !== "string" || !ref.startsWith(prefix)) {
    throw new Error(`unsupported non-local schema reference: ${String(ref)}`);
  }
  const definitionName = ref.slice(prefix.length);
  const generatedName = definitionTypeNames.get(definitionName);
  if (!generatedName) throw new Error(`unknown generated definition ${definitionName}`);
  return generatedName;
};

const isNullSchema = (node) => node?.type === "null" || node?.const === null;

function unionNodes(node) {
  if (Array.isArray(node?.oneOf)) return node.oneOf;
  if (Array.isArray(node?.anyOf)) return node.anyOf;
  if (Array.isArray(node?.type)) return node.type.map((type) => ({ type }));
  return null;
}

function typeFor(node, context) {
  if (!node || typeof node !== "object") throw new Error(`invalid schema node for ${context}`);
  if (Object.keys(node).length === 0) return { kind: "never" };
  if (node.$ref) return { kind: "ref", name: refName(node.$ref) };
  if (typeof node.const === "string") {
    register(context, { enum: [node.const] });
    return { kind: "ref", name: context };
  }
  if (typeof node.const === "boolean") return { kind: "boolean", constValue: node.const };
  if (typeof node.const === "number" && Number.isSafeInteger(node.const)) {
    return { kind: "integer", minimum: node.const, maximum: node.const };
  }
  if (node.const === null) return { kind: "null" };
  if (Array.isArray(node.enum) && node.enum.every((value) => typeof value === "string")) {
    register(context, node);
    return { kind: "ref", name: context };
  }
  const union = unionNodes(node);
  if (union && !node.properties && node.type !== "object") {
    const nonNull = union.filter((entry) => !isNullSchema(entry));
    if (nonNull.length + 1 === union.length && nonNull.length === 1) {
      return { kind: "optional", inner: typeFor(nonNull[0], `${context}Value`) };
    }
    const primitiveKinds = union.map((entry) => entry.type).filter(Boolean);
    if (primitiveKinds.length === union.length && new Set(primitiveKinds).size === 1) {
      return typeFor({ type: primitiveKinds[0] }, context);
    }
    register(context, node);
    return { kind: "ref", name: context };
  }
  if (node.type === "string") return { kind: "string" };
  if (node.type === "integer") {
    const minimum = Number.isSafeInteger(node.minimum) ? node.minimum : -9007199254740991;
    const maximum = Number.isSafeInteger(node.maximum) ? node.maximum : 9007199254740991;
    if (minimum > maximum) throw new Error(`invalid integer range for ${context}`);
    return { kind: "integer", minimum, maximum };
  }
  if (node.type === "number") throw new Error(`unsupported fractional number schema for ${context}`);
  if (node.type === "boolean") return { kind: "boolean" };
  if (node.type === "null") return { kind: "null" };
  if (node.type === "array") {
    return { kind: "array", item: typeFor(node.items ?? {}, `${context}Item`) };
  }
  if (node.type === "object" || node.properties || node.additionalProperties) {
    if (!node.properties && node.additionalProperties && typeof node.additionalProperties === "object") {
      return { kind: "map", value: typeFor(node.additionalProperties, `${context}Value`) };
    }
    if (node.properties && node.additionalProperties !== false) {
      throw new Error(`${context} object schemas with properties must set additionalProperties to false`);
    }
    register(context, node);
    return { kind: "ref", name: context };
  }
  throw new Error(`unsupported schema shape for ${context}`);
}

function variantName(context, node, index) {
  if (node.$ref) return refName(node.$ref);
  const discriminator = Object.values(node.properties ?? {})
    .find((property) => typeof property?.const === "string")?.const;
  return `${context}${discriminator ? enumVariant(discriminator) : `Variant${index + 1}`}`;
}

function register(name, node) {
  if (declarations.has(name)) {
    if (declarationSources.get(name) !== node) {
      throw new Error(`generated declaration name collision at ${name}`);
    }
    return;
  }
  declarationSources.set(name, node);
  declarations.set(name, { kind: "pending", name });
  if (name === "ItemDataValue") {
    declarations.set(name, { kind: "json", name });
    return;
  }
  if (Array.isArray(node.enum) && node.enum.every((value) => typeof value === "string")) {
    const variants = node.enum.map(enumVariant);
    if (new Set(variants).size !== variants.length) {
      throw new Error(`generated enum variant collision in ${name}`);
    }
    declarations.set(name, { kind: "enum", name, values: [...node.enum] });
    return;
  }
  if (node.type === "object" || node.properties) {
    if (node.properties && node.additionalProperties !== false) {
      throw new Error(`${name} object schemas with properties must set additionalProperties to false`);
    }
    const required = new Set(node.required ?? []);
    const fields = Object.entries(node.properties ?? {})
      .sort(([left], [right]) => compareOrdinal(left, right))
      .map(([jsonName, property]) => ({
        jsonName,
        fieldName: rustField(snake(jsonName)),
        cppFieldName: cppField(snake(jsonName)),
        required: required.has(jsonName),
        type: typeFor(property, `${name}${pascal(jsonName)}`),
      }));
    for (const generatedField of ["fieldName", "cppFieldName"]) {
      const seen = new Set();
      for (const field of fields) {
        if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(field[generatedField])) {
          throw new Error(`invalid generated ${generatedField} in ${name} at ${field.jsonName}`);
        }
        if (seen.has(field[generatedField])) {
          throw new Error(`generated ${generatedField} collision in ${name} at ${field.jsonName}`);
        }
        seen.add(field[generatedField]);
      }
    }
    declarations.set(name, { kind: "object", name, fields });
    return;
  }
  const union = unionNodes(node);
  if (union) {
    const nonNull = union.filter((entry) => !isNullSchema(entry));
    if (nonNull.length + 1 === union.length && nonNull.length === 1) {
      declarations.set(name, {
        kind: "alias",
        name,
        type: { kind: "optional", inner: typeFor(nonNull[0], `${name}Value`) },
      });
      return;
    }
    const rendered = nonNull.map((entry, index) => {
      const variant = variantName(name, entry, index);
      return { name: variant, type: typeFor(entry, variant) };
    });
    const variantNames = rendered.map((entry) => enumVariant(entry.name));
    if (new Set(variantNames).size !== variantNames.length) {
      throw new Error(`generated union variant collision in ${name}`);
    }
    const signatures = rendered.map((entry) => JSON.stringify(entry.type));
    if (new Set(signatures).size === 1) {
      declarations.set(name, { kind: "alias", name, type: rendered[0].type });
    } else {
      declarations.set(name, { kind: "union", name, variants: rendered });
    }
    return;
  }
  declarations.set(name, { kind: "alias", name, type: typeFor(node, `${name}Value`) });
}

for (const [name, node] of Object.entries(definitions)) register(definitionTypeNames.get(name), node);

const fixtureEntries = fixtureMap.entries.map((entry) => {
  if (!definitions[entry.definition]) throw new Error(`fixture map references ${entry.definition}`);
  return {
    jsonName: entry.key,
    fieldName: rustField(snake(entry.key)),
    cppFieldName: cppField(snake(entry.key)),
    required: true,
    type: entry.array
      ? { kind: "array", item: { kind: "ref", name: definitionTypeNames.get(entry.definition) } }
      : { kind: "ref", name: definitionTypeNames.get(entry.definition) },
  };
}).sort((left, right) => compareOrdinal(left.jsonName, right.jsonName));
for (const fieldName of ["fieldName", "cppFieldName"]) {
  const generatedNames = new Set();
  for (const entry of fixtureEntries) {
    if (generatedNames.has(entry[fieldName])) {
      throw new Error(`fixture map has a generated ${fieldName} collision at ${entry.jsonName}`);
    }
    generatedNames.add(entry[fieldName]);
  }
}
declarations.set("CoreFixtureCatalog", { kind: "object", name: "CoreFixtureCatalog", fields: fixtureEntries });

if ([...declarations.values()].some((entry) => entry.kind === "pending")) {
  throw new Error("generator left an unresolved declaration");
}

const rustType = (type) => {
  switch (type.kind) {
    case "ref": return type.name;
    case "string": return "String";
    case "integer": return "i64";
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

function renderRust() {
  const out = [
    "// @generated by agent-runtime/aap-schema/scripts/generate-core-types.mjs; do not edit.",
    "use serde::{Deserialize, Serialize};",
    "use serde_json::Value;",
    "",
    `pub const CORE_SCHEMA_ID: &str = ${JSON.stringify(expectedSchemaId)};`,
    `pub const CORE_SCHEMA_SHA256: &str = ${JSON.stringify(schemaSha256)};`,
    `pub const CORE_SCHEMA_JSON: &str = ${JSON.stringify(JSON.stringify(schema))};`,
    "",
  ];
  for (const declaration of declarations.values()) {
    if (declaration.kind === "json") {
      out.push(`pub type ${declaration.name} = serde_json::Value;`, "");
    } else if (declaration.kind === "alias") {
      out.push(`pub type ${declaration.name} = ${rustType(declaration.type)};`, "");
    } else if (declaration.kind === "enum") {
      out.push("#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]", `pub enum ${declaration.name} {`);
      for (const value of declaration.values) {
        out.push(`    #[serde(rename = ${JSON.stringify(value)})]`, `    ${enumVariant(value)},`);
      }
      out.push("}", "");
    } else if (declaration.kind === "union") {
      out.push("#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]", "#[serde(untagged)]", `pub enum ${declaration.name} {`);
      for (const variant of declaration.variants) out.push(`    ${enumVariant(variant.name)}(${rustType(variant.type)}),`);
      out.push("}", "");
    } else if (declaration.kind === "object") {
      out.push(
        "#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]",
        "#[serde(deny_unknown_fields)]",
        `pub struct ${declaration.name} {`,
      );
      for (const field of declaration.fields) {
        if (field.fieldName !== field.jsonName) out.push(`    #[serde(rename = ${JSON.stringify(field.jsonName)})]`);
        if (!field.required) out.push("    #[serde(default, skip_serializing_if = \"Option::is_none\")]");
        const fieldType = field.required ? rustType(field.type) : `Option<${rustType(field.type)}>`;
        out.push(`    pub ${field.fieldName}: ${fieldType},`);
      }
      out.push("}", "");
    }
  }
  out.push(
    "fn core_validation_error() -> serde_json::Error {",
    "    serde_json::Error::io(std::io::Error::new(std::io::ErrorKind::InvalidData, \"value does not match generated core schema\"))",
    "}",
    "",
    "fn validate_item_data_value_inner(value: &Value, depth: usize, count: &mut usize) -> Result<(), serde_json::Error> {",
    "    if depth > 16 { return Err(core_validation_error()); }",
    "    *count = count.checked_add(1).ok_or_else(core_validation_error)?;",
    "    if *count > 4096 { return Err(core_validation_error()); }",
    "    match value {",
    "        Value::Null | Value::Bool(_) | Value::String(_) => Ok(()),",
    "        Value::Number(number) => {",
    "            let number = number.as_f64().ok_or_else(core_validation_error)?;",
    "            if !number.is_finite() || number.fract() != 0.0 || number.abs() > 9_007_199_254_740_991.0 { return Err(core_validation_error()); }",
    "            Ok(())",
    "        }",
    "        Value::Array(values) => values.iter().try_for_each(|entry| validate_item_data_value_inner(entry, depth + 1, count)),",
    "        Value::Object(values) => values.values().try_for_each(|entry| validate_item_data_value_inner(entry, depth + 1, count)),",
    "    }",
    "}",
    "",
    "fn validate_item_data_value(value: &Value) -> Result<(), serde_json::Error> {",
    "    validate_item_data_value_inner(value, 0, &mut 0)",
    "}",
    "",
    "fn schema_matches(root: &Value, rule: &Value, value: &Value) -> bool {",
    "    let document = serde_json::json!({",
    "        \"$schema\": root.get(\"$schema\").cloned().unwrap_or(Value::String(\"https://json-schema.org/draft/2020-12/schema\".to_owned())),",
    "        \"$defs\": root.get(\"$defs\").cloned().unwrap_or_else(|| Value::Object(Default::default())),",
    "        \"allOf\": [rule.clone()],",
    "    });",
    "    jsonschema::validator_for(&document).is_ok_and(|validator| validator.is_valid(value))",
    "}",
    "",
    "fn validate_item_data_references(root: &Value, rule: &Value, value: &Value, depth: usize) -> Result<(), serde_json::Error> {",
    "    if depth > 128 { return Err(core_validation_error()); }",
    "    let rule = rule.as_object().ok_or_else(core_validation_error)?;",
    "    let definitions = root.get(\"$defs\").and_then(Value::as_object).ok_or_else(core_validation_error)?;",
    "    if let Some(reference) = rule.get(\"$ref\").and_then(Value::as_str) {",
    "        let name = reference.strip_prefix(\"#/$defs/\").ok_or_else(core_validation_error)?;",
    "        if name == \"itemDataValue\" { return validate_item_data_value(value); }",
    "        let target = definitions.get(name).ok_or_else(core_validation_error)?;",
    "        return validate_item_data_references(root, target, value, depth + 1);",
    "    }",
    "    for keyword in [\"allOf\", \"anyOf\", \"oneOf\"] {",
    "        if let Some(children) = rule.get(keyword).and_then(Value::as_array) {",
    "            for child in children.iter().filter(|child| schema_matches(root, child, value)) {",
    "                validate_item_data_references(root, child, value, depth + 1)?;",
    "            }",
    "        }",
    "    }",
    "    if let Some(condition) = rule.get(\"if\") {",
    "        let matched = schema_matches(root, condition, value);",
    "        if matched { validate_item_data_references(root, condition, value, depth + 1)?; }",
    "        if let Some(branch) = rule.get(if matched { \"then\" } else { \"else\" }) {",
    "            validate_item_data_references(root, branch, value, depth + 1)?;",
    "        }",
    "    }",
    "    if let Some(values) = value.as_array() {",
    "        if let Some(items) = rule.get(\"items\") {",
    "            for entry in values { validate_item_data_references(root, items, entry, depth + 1)?; }",
    "        }",
    "        if let Some(contains) = rule.get(\"contains\") {",
    "            for entry in values.iter().filter(|entry| schema_matches(root, contains, entry)) {",
    "                validate_item_data_references(root, contains, entry, depth + 1)?;",
    "            }",
    "        }",
    "    }",
    "    if let Some(values) = value.as_object() {",
    "        let properties = rule.get(\"properties\").and_then(Value::as_object);",
    "        if let Some(properties) = properties {",
    "            for (key, child) in properties {",
    "                if let Some(entry) = values.get(key) { validate_item_data_references(root, child, entry, depth + 1)?; }",
    "            }",
    "        }",
    "        if let Some(additional) = rule.get(\"additionalProperties\").filter(|entry| entry.is_object()) {",
    "            for (key, entry) in values {",
    "                if properties.is_none_or(|known| !known.contains_key(key)) {",
    "                    validate_item_data_references(root, additional, entry, depth + 1)?;",
    "                }",
    "            }",
    "        }",
    "    }",
    "    Ok(())",
    "}",
    "",
    "pub fn validate_core_definition(name: &str, value: &Value) -> Result<(), serde_json::Error> {",
    "    let root: Value = serde_json::from_str(CORE_SCHEMA_JSON)?;",
    "    let definitions = root.get(\"$defs\").and_then(Value::as_object).ok_or_else(core_validation_error)?;",
    "    if !definitions.contains_key(name) { return Err(core_validation_error()); }",
    "    let document = serde_json::json!({",
    "        \"$schema\": root.get(\"$schema\").cloned().unwrap_or(Value::String(\"https://json-schema.org/draft/2020-12/schema\".to_owned())),",
    "        \"$defs\": Value::Object(definitions.clone()),",
    "        \"$ref\": format!(\"#/$defs/{name}\"),",
    "    });",
    "    let validator = jsonschema::validator_for(&document).map_err(|_| core_validation_error())?;",
    "    validator.validate(value).map_err(|_| core_validation_error())?;",
    "    validate_item_data_references(&root, &document, value, 0)",
    "}",
    "",
    "fn validate_core_fixture_value(value: &Value) -> Result<(), serde_json::Error> {",
    "    let object = value.as_object().ok_or_else(core_validation_error)?;",
  );
  for (const entry of fixtureMap.entries) {
    if (entry.array) {
      out.push(
        `    for item in object.get(${JSON.stringify(entry.key)}).and_then(Value::as_array).ok_or_else(core_validation_error)? {`,
        `        validate_core_definition(${JSON.stringify(entry.definition)}, item)?;`,
        "    }",
      );
    } else {
      out.push(`    validate_core_definition(${JSON.stringify(entry.definition)}, object.get(${JSON.stringify(entry.key)}).ok_or_else(core_validation_error)?)?;`);
    }
  }
  out.push(
    "    Ok(())",
    "}",
    "",
    "fn sort_json(value: serde_json::Value) -> serde_json::Value {",
    "    match value {",
    "        serde_json::Value::Array(values) => serde_json::Value::Array(values.into_iter().map(sort_json).collect()),",
    "        serde_json::Value::Object(values) => {",
    "            let mut entries: Vec<_> = values.into_iter().collect();",
    "            entries.sort_by(|left, right| left.0.cmp(&right.0));",
    "            serde_json::Value::Object(entries.into_iter().map(|(key, value)| (key, sort_json(value))).collect())",
    "        }",
    "        scalar => scalar,",
    "    }",
    "}",
    "",
    "pub fn decode_core_fixture_catalog(bytes: &[u8]) -> Result<CoreFixtureCatalog, serde_json::Error> {",
    "    let value: Value = serde_json::from_slice(bytes)?;",
    "    validate_core_fixture_value(&value)?;",
    "    serde_json::from_value(value)",
    "}",
    "",
    "pub fn canonical_core_fixture_catalog(value: &CoreFixtureCatalog) -> Result<Vec<u8>, serde_json::Error> {",
    "    let encoded = serde_json::to_value(value)?;",
    "    validate_core_fixture_value(&encoded)?;",
    "    serde_json::to_vec(&sort_json(encoded))",
    "}",
    "",
  );
  const rendered = out.join("\n").replace(
    /^(pub (?:const|type|enum|struct|fn)|fn )/gm,
    "#[rustfmt::skip]\n$&",
  );
  return `${rendered.trimEnd()}\n`;
}

const tsType = (type) => {
  switch (type.kind) {
    case "ref": return type.name;
    case "string": return "string";
    case "integer": return "number";
    case "boolean": return Object.hasOwn(type, "constValue") ? String(type.constValue) : "boolean";
    case "null": return "null";
    case "json": return "JsonValue";
    case "never": return "never";
    case "array": return `ReadonlyArray<${tsType(type.item)}>`;
    case "map": return `Readonly<Record<string, ${tsType(type.value)}>>`;
    case "optional": return `${tsType(type.inner)} | null`;
    default: throw new Error(`unsupported TypeScript type ${type.kind}`);
  }
};

function renderTypeScriptDeclarations() {
  const out = [
    "// @generated by agent-runtime/aap-schema/scripts/generate-core-types.mjs; do not edit.",
    "export type JsonValue = null | boolean | number | string | ReadonlyArray<JsonValue> | Readonly<Record<string, JsonValue>>;",
    `export declare const CORE_SCHEMA_ID: ${JSON.stringify(expectedSchemaId)};`,
    `export declare const CORE_SCHEMA_SHA256: ${JSON.stringify(schemaSha256)};`,
    "",
  ];
  for (const declaration of declarations.values()) {
    if (declaration.kind === "json") {
      out.push(`export type ${declaration.name} = JsonValue;`, "");
    } else if (declaration.kind === "alias") {
      out.push(`export type ${declaration.name} = ${tsType(declaration.type)};`, "");
    } else if (declaration.kind === "enum") {
      out.push(`export type ${declaration.name} = ${declaration.values.map(JSON.stringify).join(" | ")};`, "");
    } else if (declaration.kind === "union") {
      out.push(`export type ${declaration.name} = ${declaration.variants.map((variant) => tsType(variant.type)).join(" | ")};`, "");
    } else if (declaration.kind === "object") {
      out.push(`export interface ${declaration.name} {`);
      for (const field of declaration.fields) {
        const key = /^[A-Za-z_$][A-Za-z0-9_$]*$/.test(field.jsonName) ? field.jsonName : JSON.stringify(field.jsonName);
        out.push(`  readonly ${key}${field.required ? "" : "?"}: ${tsType(field.type)};`);
      }
      out.push("}", "");
    }
  }
  out.push(
    "export declare function validateCoreDefinition(name: string, input: unknown): void;",
    "export declare function decodeCoreFixtureCatalog(input: unknown): CoreFixtureCatalog;",
    "export declare function canonicalCoreFixtureCatalog(value: CoreFixtureCatalog): string;",
    "",
  );
  return `${out.join("\n").replace(/\n+$/u, "")}\n`;
}

function descriptorType(type) {
  if (type.kind === "ref") return { kind: "ref", name: type.name };
  if (type.kind === "array") return { kind: "array", item: descriptorType(type.item) };
  if (type.kind === "map") return { kind: "map", value: descriptorType(type.value) };
  if (type.kind === "optional") return { kind: "optional", inner: descriptorType(type.inner) };
  if (type.kind === "integer") return { kind: "integer", minimum: type.minimum, maximum: type.maximum };
  if (type.kind === "boolean" && Object.hasOwn(type, "constValue")) {
    return { kind: "boolean", constValue: type.constValue };
  }
  return { kind: type.kind };
}

function descriptorDeclaration(declaration) {
  if (declaration.kind === "object") return {
    kind: "object",
    fields: declaration.fields.map((field) => ({
      jsonName: field.jsonName,
      required: field.required,
      type: descriptorType(field.type),
    })),
  };
  if (declaration.kind === "enum") return { kind: "enum", values: declaration.values };
  if (declaration.kind === "union") return { kind: "union", variants: declaration.variants.map((variant) => descriptorType(variant.type)) };
  if (declaration.kind === "alias") return { kind: "alias", type: descriptorType(declaration.type) };
  return { kind: declaration.kind };
}

function renderTypeScriptRuntime() {
  const descriptors = Object.fromEntries([...declarations].map(([name, declaration]) => [name, descriptorDeclaration(declaration)]));
  const fixtureDefinitions = Object.fromEntries(fixtureMap.entries.map((entry) => [entry.key, {
    definition: entry.definition,
    array: entry.array === true,
  }]));
  return `// @generated by agent-runtime/aap-schema/scripts/generate-core-types.mjs; do not edit.\n` +
`export const CORE_SCHEMA_ID = ${JSON.stringify(expectedSchemaId)};\n` +
`export const CORE_SCHEMA_SHA256 = ${JSON.stringify(schemaSha256)};\n` +
`const descriptors = ${JSON.stringify(descriptors, null, 2)};\n` +
`const coreSchema = ${JSON.stringify(schema, null, 2)};\n` +
`const fixtureDefinitions = ${JSON.stringify(fixtureDefinitions, null, 2)};\n` +
String.raw`
const equalJson = (left, right) => JSON.stringify(sortJson(left)) === JSON.stringify(sortJson(right));

const isUnicodeScalarString = (value) => {
  for (let index = 0; index < value.length; index += 1) {
    const codeUnit = value.charCodeAt(index);
    if (codeUnit >= 0xd800 && codeUnit <= 0xdbff) {
      if (index + 1 >= value.length) return false;
      const next = value.charCodeAt(index + 1);
      if (next < 0xdc00 || next > 0xdfff) return false;
      index += 1;
    } else if (codeUnit >= 0xdc00 && codeUnit <= 0xdfff) {
      return false;
    }
  }
  return true;
};

const hasOnlyUnicodeScalars = (value, depth = 0, seen = new Set()) => {
  if (depth > 128) return false;
  if (typeof value === "string") return isUnicodeScalarString(value);
  if (!value || typeof value !== "object") return true;
  if (seen.has(value)) return false;
  seen.add(value);
  const valid = Array.isArray(value)
    ? value.every((entry) => hasOnlyUnicodeScalars(entry, depth + 1, seen))
    : Object.keys(value).every((key) =>
        isUnicodeScalarString(key) && hasOnlyUnicodeScalars(value[key], depth + 1, seen));
  seen.delete(value);
  return valid;
};

const validateItemDataValue = (input) => {
  const state = { count: 0 };
  const visit = (value, depth) => {
    if (depth > 16 || ++state.count > 4096) return false;
    if (value === null || typeof value === "boolean") return true;
    if (typeof value === "string") return isUnicodeScalarString(value);
    if (typeof value === "number") return Number.isSafeInteger(value);
    if (Array.isArray(value)) return value.every((entry) => visit(entry, depth + 1));
    if (value && typeof value === "object" &&
        (Object.getPrototypeOf(value) === Object.prototype || Object.getPrototypeOf(value) === null)) {
      return Object.entries(value).every(([key, entry]) =>
        isUnicodeScalarString(key) && visit(entry, depth + 1));
    }
    return false;
  };
  return visit(input, 0);
};

const matchesSchema = (rule, input, path, depth = 0) => {
  if (!rule || typeof rule !== "object" || Array.isArray(rule) || depth > 128) return false;
  if (rule.$ref) {
    const prefix = "#/$defs/";
    if (typeof rule.$ref !== "string" || !rule.$ref.startsWith(prefix)) return false;
    const name = rule.$ref.slice(prefix.length);
    if (name === "itemDataValue" && !validateItemDataValue(input)) return false;
    return matchesSchema(coreSchema.$defs[name], input, path, depth + 1);
  }
  if (Object.hasOwn(rule, "const") && !equalJson(input, rule.const)) return false;
  if (Array.isArray(rule.enum) && !rule.enum.some((entry) => equalJson(input, entry))) return false;
  if (Array.isArray(rule.oneOf) && rule.oneOf.filter((entry) => matchesSchema(entry, input, path, depth + 1)).length !== 1) return false;
  if (Array.isArray(rule.anyOf) && !rule.anyOf.some((entry) => matchesSchema(entry, input, path, depth + 1))) return false;
  if (Array.isArray(rule.allOf) && !rule.allOf.every((entry) => matchesSchema(entry, input, path, depth + 1))) return false;
  if (rule.not && matchesSchema(rule.not, input, path, depth + 1)) return false;
  if (rule.if) {
    const branch = matchesSchema(rule.if, input, path, depth + 1) ? rule.then : rule.else;
    if (branch && !matchesSchema(branch, input, path, depth + 1)) return false;
  }
  if (rule.type === "null" && input !== null) return false;
  if (rule.type === "boolean" && typeof input !== "boolean") return false;
  if (rule.type === "integer" && !Number.isSafeInteger(input)) return false;
  if (rule.type === "number" && (typeof input !== "number" || !Number.isFinite(input))) return false;
  if (rule.type === "string" && (typeof input !== "string" || !isUnicodeScalarString(input))) return false;
  if (rule.type === "array" && !Array.isArray(input)) return false;
  if (rule.type === "object" && (!input || typeof input !== "object" || Array.isArray(input))) return false;
  if (typeof input === "number") {
    if (typeof rule.minimum === "number" && input < rule.minimum) return false;
    if (typeof rule.maximum === "number" && input > rule.maximum) return false;
  }
  if (typeof input === "string") {
    const length = [...input].length;
    if (Number.isInteger(rule.minLength) && length < rule.minLength) return false;
    if (Number.isInteger(rule.maxLength) && length > rule.maxLength) return false;
    if (typeof rule.pattern === "string" && !(new RegExp(rule.pattern, "u")).test(input)) return false;
  }
  if (Array.isArray(input)) {
    if (Number.isInteger(rule.minItems) && input.length < rule.minItems) return false;
    if (Number.isInteger(rule.maxItems) && input.length > rule.maxItems) return false;
    if (rule.uniqueItems === true) {
      const identities = input.map((entry) => JSON.stringify(sortJson(entry)));
      if (new Set(identities).size !== identities.length) return false;
    }
    if (rule.items && !input.every((entry, index) => matchesSchema(rule.items, entry, path + "[" + index + "]", depth + 1))) return false;
    if (rule.contains) {
      const count = input.filter((entry, index) => matchesSchema(rule.contains, entry, path + "[" + index + "]", depth + 1)).length;
      const minimum = Number.isInteger(rule.minContains) ? rule.minContains : 1;
      const maximum = Number.isInteger(rule.maxContains) ? rule.maxContains : Number.MAX_SAFE_INTEGER;
      if (count < minimum || count > maximum) return false;
    }
  }
  if (input && typeof input === "object" && !Array.isArray(input)) {
    const keys = Object.keys(input);
    if (Number.isInteger(rule.minProperties) && keys.length < rule.minProperties) return false;
    if (Number.isInteger(rule.maxProperties) && keys.length > rule.maxProperties) return false;
    if (Array.isArray(rule.required) && !rule.required.every((key) => Object.hasOwn(input, key))) return false;
    if (rule.propertyNames && !keys.every((key) => matchesSchema(rule.propertyNames, key, path + ".<property>", depth + 1))) return false;
    const properties = rule.properties && typeof rule.properties === "object" ? rule.properties : {};
    for (const [key, child] of Object.entries(properties)) {
      if (Object.hasOwn(input, key) && !matchesSchema(child, input[key], path + "." + key, depth + 1)) return false;
    }
    for (const key of keys.filter((entry) => !Object.hasOwn(properties, entry))) {
      if (rule.additionalProperties === false) return false;
      if (rule.additionalProperties && typeof rule.additionalProperties === "object" &&
          !matchesSchema(rule.additionalProperties, input[key], path + "." + key, depth + 1)) return false;
    }
  }
  return true;
};

const decodeType = (type, input, path) => {
  if (type.kind === "ref") return decodeDeclaration(type.name, input, path);
  if (type.kind === "optional") return input === null ? null : decodeType(type.inner, input, path);
  if (type.kind === "array") {
    if (!Array.isArray(input)) throw new TypeError(path + " must be an array");
    return input.map((value, index) => decodeType(type.item, value, path + "[" + index + "]"));
  }
  if (type.kind === "map") {
    if (!input || typeof input !== "object" || Array.isArray(input)) throw new TypeError(path + " must be an object");
    return Object.fromEntries(Object.keys(input).sort().map((key) => [key, decodeType(type.value, input[key], path + "." + key)]));
  }
  if (type.kind === "string" && (typeof input !== "string" || !isUnicodeScalarString(input))) {
    throw new TypeError(path + " must be a Unicode scalar string");
  }
  if (type.kind === "integer" && (!Number.isSafeInteger(input) || input < type.minimum || input > type.maximum)) {
    throw new TypeError(path + " must be an integer in the generated range");
  }
  if (type.kind === "boolean" && (typeof input !== "boolean" || ("constValue" in type && input !== type.constValue))) {
    throw new TypeError(path + " must be the generated boolean value");
  }
  if (type.kind === "null" && input !== null) throw new TypeError(path + " must be null");
  if (type.kind === "json") return structuredClone(input);
  if (type.kind === "never") throw new TypeError(path + " cannot contain a value");
  return input;
};

const decodeDeclaration = (name, input, path) => {
  const descriptor = descriptors[name];
  if (!descriptor) throw new TypeError("unknown generated type " + name);
  if (descriptor.kind === "alias") return decodeType(descriptor.type, input, path);
  if (descriptor.kind === "json") return structuredClone(input);
  if (descriptor.kind === "enum") {
    if (!descriptor.values.includes(input)) throw new TypeError(path + " is outside " + name);
    return input;
  }
  if (descriptor.kind === "union") {
    for (const variant of descriptor.variants) {
      try { return decodeType(variant, input, path); } catch (_) { /* try the next exact variant */ }
    }
    throw new TypeError(path + " does not match " + name);
  }
  if (descriptor.kind === "object") {
    if (!input || typeof input !== "object" || Array.isArray(input)) throw new TypeError(path + " must be an object");
    const allowed = new Set(descriptor.fields.map((field) => field.jsonName));
    for (const key of Object.keys(input)) if (!allowed.has(key)) throw new TypeError(path + "." + key + " is unknown");
    const output = {};
    for (const field of descriptor.fields) {
      if (!(field.jsonName in input)) {
        if (field.required) throw new TypeError(path + "." + field.jsonName + " is required");
        continue;
      }
      output[field.jsonName] = decodeType(field.type, input[field.jsonName], path + "." + field.jsonName);
    }
    return output;
  }
  throw new TypeError("unsupported generated type " + name);
};

const sortJson = (value) => Array.isArray(value)
  ? value.map(sortJson)
  : value && typeof value === "object"
    ? Object.fromEntries(Object.keys(value).sort().map((key) => [key, sortJson(value[key])]))
    : value;

export const validateCoreDefinition = (name, input) => {
  const definition = coreSchema.$defs[name];
  if (!definition) throw new TypeError("unknown core definition " + name);
  if (!hasOnlyUnicodeScalars(input)) throw new TypeError(name + " contains invalid Unicode");
  if (name === "itemDataValue" && !validateItemDataValue(input)) {
    throw new TypeError(name + " exceeds generated ItemData limits");
  }
  if (!matchesSchema(definition, input, name)) throw new TypeError(name + " does not match core schema");
};

export const decodeCoreFixtureCatalog = (input) => {
  const decoded = decodeDeclaration("CoreFixtureCatalog", input, "fixture");
  for (const [key, mapping] of Object.entries(fixtureDefinitions)) {
    if (mapping.array) decoded[key].forEach((entry) => validateCoreDefinition(mapping.definition, entry));
    else validateCoreDefinition(mapping.definition, decoded[key]);
  }
  return decoded;
};
export const canonicalCoreFixtureCatalog = (value) => JSON.stringify(sortJson(decodeCoreFixtureCatalog(value)));
`;
}

const cppType = (type) => {
  switch (type.kind) {
    case "ref": return type.name;
    case "string": return "QString";
    case "integer": return "qint64";
    case "boolean": return "bool";
    case "null": return "std::nullptr_t";
    case "json": return "QJsonValue";
    case "never": return "QJsonValue";
    case "array": return `QList<${cppType(type.item)}>`;
    case "map": return `QMap<QString, ${cppType(type.value)}>`;
    case "optional": return `std::optional<${cppType(type.inner)}>`;
    default: throw new Error(`unsupported C++ type ${type.kind}`);
  }
};

function dependencies(type, into = new Set()) {
  if (type.kind === "ref") into.add(type.name);
  if (type.kind === "array") dependencies(type.item, into);
  if (type.kind === "map") dependencies(type.value, into);
  if (type.kind === "optional") dependencies(type.inner, into);
  return into;
}

function orderedDeclarations() {
  const remaining = new Map(declarations);
  const ordered = [];
  while (remaining.size) {
    let progressed = false;
    for (const [name, declaration] of remaining) {
      const deps = new Set();
      if (declaration.kind === "alias") dependencies(declaration.type, deps);
      if (declaration.kind === "union") declaration.variants.forEach((variant) => dependencies(variant.type, deps));
      if (declaration.kind === "object") declaration.fields.forEach((field) => dependencies(field.type, deps));
      deps.delete(name);
      if ([...deps].every((dependency) => !remaining.has(dependency))) {
        ordered.push(declaration);
        remaining.delete(name);
        progressed = true;
      }
    }
    if (!progressed) throw new Error(`cyclic generated C++ declarations: ${[...remaining.keys()].join(", ")}`);
  }
  return ordered;
}

function renderCppHeader() {
  const out = [
    "// @generated by agent-runtime/aap-schema/scripts/generate-core-types.mjs; do not edit.",
    "#pragma once",
    "#include <QByteArray>",
    "#include <QJsonValue>",
    "#include <QList>",
    "#include <QMap>",
    "#include <QString>",
    "#include <optional>",
    "#include <variant>",
    "",
    "namespace aegisy::aap::generated {",
    `inline constexpr auto kCoreSchemaId = ${JSON.stringify(expectedSchemaId)};`,
    `inline constexpr auto kCoreSchemaSha256 = ${JSON.stringify(schemaSha256)};`,
    "",
  ];
  for (const declaration of orderedDeclarations()) {
    if (declaration.kind === "enum") {
      out.push(`enum class ${declaration.name} {`, ...declaration.values.map((value) => `    ${enumVariant(value)},`), "};", "");
    } else if (declaration.kind === "json") {
      out.push(`struct ${declaration.name} { QJsonValue value; };`, "");
    } else if (declaration.kind === "alias") {
      out.push(`struct ${declaration.name} { ${cppType(declaration.type)} value; };`, "");
    } else if (declaration.kind === "union") {
      out.push(`struct ${declaration.name} {`, `    std::variant<${declaration.variants.map((variant) => cppType(variant.type)).join(", ")}> value;`, "};", "");
    } else if (declaration.kind === "object") {
      out.push(`struct ${declaration.name} {`);
      for (const field of declaration.fields) {
        const fieldType = field.required ? cppType(field.type) : `std::optional<${cppType(field.type)}>`;
        out.push(`    ${fieldType} ${field.cppFieldName};`);
      }
      out.push("};", "");
    }
  }
  out.push(
    "bool decodeCoreFixtureCatalog(const QJsonValue &value, CoreFixtureCatalog *output, QString *error = nullptr);",
    "bool validateCoreDefinition(const QString &name, const QJsonValue &value, QString *error = nullptr);",
    "QJsonValue encodeCoreFixtureCatalog(const CoreFixtureCatalog &value);",
    "QByteArray canonicalCoreFixtureCatalog(const CoreFixtureCatalog &value);",
    "",
    "} // namespace aegisy::aap::generated",
    "",
  );
  return `${out.join("\n").replace(/\n+$/u, "")}\n`;
}

function cppDecode(type, input, output, indent = "    ") {
  const lines = [];
  const fail = (message) => lines.push(`${indent}return fail(error, QStringLiteral(${JSON.stringify(message)}));`);
  if (type.kind === "ref") {
    lines.push(`${indent}if (!decodeValue(${input}, &${output}, error)) return false;`);
  } else if (type.kind === "string") {
    lines.push(`${indent}if (!${input}.isString()) {`); fail("expected string"); lines.push(`${indent}}`, `${indent}${output} = ${input}.toString();`);
  } else if (type.kind === "integer") {
    lines.push(`${indent}if (!decodeSafeInteger(${input}, ${type.minimum}, ${type.maximum}, &${output})) {`); fail("expected integer in generated range"); lines.push(`${indent}}`);
  } else if (type.kind === "boolean") {
    const constCheck = Object.hasOwn(type, "constValue") ? ` || ${input}.toBool() != ${type.constValue}` : "";
    lines.push(`${indent}if (!${input}.isBool()${constCheck}) {`); fail("expected generated boolean value"); lines.push(`${indent}}`, `${indent}${output} = ${input}.toBool();`);
  } else if (type.kind === "null") {
    lines.push(`${indent}if (!${input}.isNull()) {`); fail("expected null"); lines.push(`${indent}}`, `${indent}${output} = nullptr;`);
  } else if (type.kind === "json") {
    lines.push(`${indent}${output} = ${input};`);
  } else if (type.kind === "never") {
    fail("unexpected value in empty-only array");
  } else if (type.kind === "optional") {
    lines.push(`${indent}if (${input}.isNull()) {`, `${indent}    ${output}.reset();`, `${indent}} else {`, `${indent}    ${cppType(type.inner)} decoded;`);
    lines.push(...cppDecode(type.inner, input, "decoded", `${indent}    `));
    lines.push(`${indent}    ${output} = std::move(decoded);`, `${indent}}`);
  } else if (type.kind === "array") {
    lines.push(`${indent}if (!${input}.isArray()) {`); fail("expected array"); lines.push(`${indent}}`, `${indent}${output}.clear();`, `${indent}for (const auto &item : ${input}.toArray()) {`, `${indent}    ${cppType(type.item)} itemDecoded;`);
    lines.push(...cppDecode(type.item, "item", "itemDecoded", `${indent}    `));
    lines.push(`${indent}    ${output}.append(std::move(itemDecoded));`, `${indent}}`);
  } else if (type.kind === "map") {
    lines.push(`${indent}if (!${input}.isObject()) {`); fail("expected object map"); lines.push(`${indent}}`, `${indent}${output}.clear();`, `${indent}const auto mapped = ${input}.toObject();`, `${indent}for (auto it = mapped.begin(); it != mapped.end(); ++it) {`, `${indent}    ${cppType(type.value)} entryDecoded;`);
    lines.push(...cppDecode(type.value, "it.value()", "entryDecoded", `${indent}    `));
    lines.push(`${indent}    ${output}.insert(it.key(), std::move(entryDecoded));`, `${indent}}`);
  }
  return lines;
}

function cppEncode(type, expression) {
  if (type.kind === "ref") return `encodeValue(${expression})`;
  if (type.kind === "string") return `QJsonValue(${expression})`;
  if (type.kind === "integer") return `QJsonValue(static_cast<double>(${expression}))`;
  if (type.kind === "boolean") return `QJsonValue(${expression})`;
  if (type.kind === "null") return "QJsonValue(QJsonValue::Null)";
  if (type.kind === "json") return expression;
  if (type.kind === "never") return expression;
  if (type.kind === "optional") return `${expression}.has_value() ? ${cppEncode(type.inner, `*${expression}`)} : QJsonValue(QJsonValue::Null)`;
  if (type.kind === "array") return `encodeArray(${expression})`;
  if (type.kind === "map") return `encodeMap(${expression})`;
  throw new Error(`unsupported C++ encoder ${type.kind}`);
}

function renderCppSource() {
  // MSVC rejects a single string literal above 16380 characters (C2026), so
  // the embedded schema is split into adjacent concatenated literals. Chunks
  // are split only at ASCII boundaries to keep every literal valid UTF-8.
  const schemaChunks = [];
  const schemaText = JSON.stringify(cppSchema);
  for (let offset = 0; offset < schemaText.length;) {
    let end = Math.min(offset + 7000, schemaText.length);
    while (end < schemaText.length && schemaText.charCodeAt(end) > 0x7f) end += 1;
    if (end > offset && end < schemaText.length) {
      const last = schemaText.charCodeAt(end - 1);
      if (last >= 0xd800 && last <= 0xdbff) end += 1;
    }
    schemaChunks.push(schemaText.slice(offset, end));
    offset = end;
  }
  const schemaLiteral = schemaChunks.map((chunk) => JSON.stringify(chunk)).join("\n        ");
  const out = [
    "// @generated by agent-runtime/aap-schema/scripts/generate-core-types.mjs; do not edit.",
    "#include \"aap_core_types_generated.h\"",
    "#include <QJsonArray>",
    "#include <QJsonDocument>",
    "#include <QJsonObject>",
    "#include <QRegularExpression>",
    "#include <QSet>",
    "#include <cmath>",
    "#include <limits>",
    "#include <utility>",
    "",
    "namespace aegisy::aap::generated {",
    "namespace {",
    "bool fail(QString *error, const QString &message) { if (error) *error = message; return false; }",
    `const QJsonObject &coreSchema() { static const char json[] =\n        ${schemaLiteral};\n    static const QJsonObject value = QJsonDocument::fromJson(QByteArray(json, sizeof(json) - 1)).object(); return value; }`,
    "bool isUnicodeScalarString(const QString &value) {",
    "    for (qsizetype index = 0; index < value.size(); ++index) {",
    "        const QChar current = value.at(index);",
    "        if (current.isHighSurrogate()) {",
    "            if (++index >= value.size() || !value.at(index).isLowSurrogate()) return false;",
    "        } else if (current.isLowSurrogate()) return false;",
    "    }",
    "    return true;",
    "}",
    "bool hasOnlyUnicodeScalars(const QJsonValue &value, int depth = 0) {",
    "    if (depth > 128) return false;",
    "    if (value.isString()) return isUnicodeScalarString(value.toString());",
    "    if (value.isArray()) { for (const auto &entry : value.toArray()) if (!hasOnlyUnicodeScalars(entry, depth + 1)) return false; }",
    "    if (value.isObject()) { const auto object = value.toObject(); for (auto it = object.begin(); it != object.end(); ++it) if (!isUnicodeScalarString(it.key()) || !hasOnlyUnicodeScalars(it.value(), depth + 1)) return false; }",
    "    return true;",
    "}",
    "bool validateItemDataValueInner(const QJsonValue &value, int depth, qsizetype *count) {",
    "    if (depth > 16 || ++(*count) > 4096) return false;",
    "    if (value.isNull() || value.isBool()) return true;",
    "    if (value.isString()) return isUnicodeScalarString(value.toString());",
    "    if (value.isDouble()) { const double number = value.toDouble(); return std::isfinite(number) && std::floor(number) == number && std::abs(number) <= 9007199254740991.0; }",
    "    if (value.isArray()) { for (const auto &entry : value.toArray()) if (!validateItemDataValueInner(entry, depth + 1, count)) return false; return true; }",
    "    if (value.isObject()) { const auto object = value.toObject(); for (auto it = object.begin(); it != object.end(); ++it) if (!validateItemDataValueInner(it.value(), depth + 1, count)) return false; return true; }",
    "    return false;",
    "}",
    "bool validateItemDataValue(const QJsonValue &value) { qsizetype count = 0; return validateItemDataValueInner(value, 0, &count); }",
    "bool matchesSchema(const QJsonObject &rule, const QJsonValue &input, int depth = 0) {",
    "    if (depth > 128) return false;",
    "    const auto root = coreSchema();",
    "    if (rule.contains(QStringLiteral(\"$ref\"))) {",
    "        const QString ref = rule.value(QStringLiteral(\"$ref\")).toString();",
    "        const QString prefix = QStringLiteral(\"#/$defs/\");",
    "        if (!ref.startsWith(prefix)) return false;",
    "        if (ref == QStringLiteral(\"#/$defs/itemDataValue\") && !validateItemDataValue(input)) return false;",
    "        const QJsonValue target = root.value(QStringLiteral(\"$defs\")).toObject().value(ref.mid(prefix.size()));",
    "        return target.isObject() && matchesSchema(target.toObject(), input, depth + 1);",
    "    }",
    "    if (rule.contains(QStringLiteral(\"const\")) && input != rule.value(QStringLiteral(\"const\"))) return false;",
    "    if (rule.value(QStringLiteral(\"enum\")).isArray()) { bool found = false; for (const auto &entry : rule.value(QStringLiteral(\"enum\")).toArray()) if (entry == input) { found = true; break; } if (!found) return false; }",
    "    if (rule.value(QStringLiteral(\"oneOf\")).isArray()) { int matches = 0; for (const auto &entry : rule.value(QStringLiteral(\"oneOf\")).toArray()) if (entry.isObject() && matchesSchema(entry.toObject(), input, depth + 1)) ++matches; if (matches != 1) return false; }",
    "    if (rule.value(QStringLiteral(\"anyOf\")).isArray()) { bool matched = false; for (const auto &entry : rule.value(QStringLiteral(\"anyOf\")).toArray()) if (entry.isObject() && matchesSchema(entry.toObject(), input, depth + 1)) { matched = true; break; } if (!matched) return false; }",
    "    if (rule.value(QStringLiteral(\"allOf\")).isArray()) for (const auto &entry : rule.value(QStringLiteral(\"allOf\")).toArray()) if (!entry.isObject() || !matchesSchema(entry.toObject(), input, depth + 1)) return false;",
    "    if (rule.value(QStringLiteral(\"not\")).isObject() && matchesSchema(rule.value(QStringLiteral(\"not\")).toObject(), input, depth + 1)) return false;",
    "    if (rule.value(QStringLiteral(\"if\")).isObject()) { const bool condition = matchesSchema(rule.value(QStringLiteral(\"if\")).toObject(), input, depth + 1); const QJsonValue branch = rule.value(condition ? QStringLiteral(\"then\") : QStringLiteral(\"else\")); if (branch.isObject() && !matchesSchema(branch.toObject(), input, depth + 1)) return false; }",
    "    const QString type = rule.value(QStringLiteral(\"type\")).toString();",
    "    if (type == QStringLiteral(\"null\") && !input.isNull()) return false;",
    "    if (type == QStringLiteral(\"boolean\") && !input.isBool()) return false;",
    "    if ((type == QStringLiteral(\"integer\") || type == QStringLiteral(\"number\")) && !input.isDouble()) return false;",
    "    if (type == QStringLiteral(\"integer\") && input.isDouble() && (!std::isfinite(input.toDouble()) || std::floor(input.toDouble()) != input.toDouble())) return false;",
    "    if (type == QStringLiteral(\"string\") && !input.isString()) return false;",
    "    if (type == QStringLiteral(\"array\") && !input.isArray()) return false;",
    "    if (type == QStringLiteral(\"object\") && !input.isObject()) return false;",
    "    if (input.isDouble()) { const double number = input.toDouble(); if (rule.value(QStringLiteral(\"minimum\")).isDouble() && number < rule.value(QStringLiteral(\"minimum\")).toDouble()) return false; if (rule.value(QStringLiteral(\"maximum\")).isDouble() && number > rule.value(QStringLiteral(\"maximum\")).toDouble()) return false; }",
    "    if (input.isString()) { const int length = input.toString().toUcs4().size(); if (rule.value(QStringLiteral(\"minLength\")).isDouble() && length < rule.value(QStringLiteral(\"minLength\")).toInt()) return false; if (rule.value(QStringLiteral(\"maxLength\")).isDouble() && length > rule.value(QStringLiteral(\"maxLength\")).toInt()) return false; if (rule.value(QStringLiteral(\"pattern\")).isString()) { const QRegularExpression pattern(rule.value(QStringLiteral(\"pattern\")).toString()); if (!pattern.isValid() || !pattern.match(input.toString()).hasMatch()) return false; } }",
    "    if (input.isArray()) {",
    "        const auto array = input.toArray(); const int size = array.size();",
    "        if (rule.value(QStringLiteral(\"minItems\")).isDouble() && size < rule.value(QStringLiteral(\"minItems\")).toInt()) return false;",
    "        if (rule.value(QStringLiteral(\"maxItems\")).isDouble() && size > rule.value(QStringLiteral(\"maxItems\")).toInt()) return false;",
    "        if (rule.value(QStringLiteral(\"uniqueItems\")).toBool(false)) { QSet<QByteArray> seen; for (const auto &entry : array) { const QByteArray encoded = QJsonDocument(QJsonArray{entry}).toJson(QJsonDocument::Compact); if (seen.contains(encoded)) return false; seen.insert(encoded); } }",
    "        if (rule.value(QStringLiteral(\"items\")).isObject()) for (const auto &entry : array) if (!matchesSchema(rule.value(QStringLiteral(\"items\")).toObject(), entry, depth + 1)) return false;",
    "        if (rule.value(QStringLiteral(\"contains\")).isObject()) { int count = 0; for (const auto &entry : array) if (matchesSchema(rule.value(QStringLiteral(\"contains\")).toObject(), entry, depth + 1)) ++count; const int minimum = rule.value(QStringLiteral(\"minContains\")).isDouble() ? rule.value(QStringLiteral(\"minContains\")).toInt() : 1; const int maximum = rule.value(QStringLiteral(\"maxContains\")).isDouble() ? rule.value(QStringLiteral(\"maxContains\")).toInt() : std::numeric_limits<int>::max(); if (count < minimum || count > maximum) return false; }",
    "    }",
    "    if (input.isObject()) {",
    "        const auto object = input.toObject(); const int size = object.size();",
    "        if (rule.value(QStringLiteral(\"minProperties\")).isDouble() && size < rule.value(QStringLiteral(\"minProperties\")).toInt()) return false;",
    "        if (rule.value(QStringLiteral(\"maxProperties\")).isDouble() && size > rule.value(QStringLiteral(\"maxProperties\")).toInt()) return false;",
    "        if (rule.value(QStringLiteral(\"required\")).isArray()) for (const auto &required : rule.value(QStringLiteral(\"required\")).toArray()) if (!object.contains(required.toString())) return false;",
    "        if (rule.value(QStringLiteral(\"propertyNames\")).isObject()) for (auto it = object.begin(); it != object.end(); ++it) if (!matchesSchema(rule.value(QStringLiteral(\"propertyNames\")).toObject(), it.key(), depth + 1)) return false;",
    "        const auto properties = rule.value(QStringLiteral(\"properties\")).toObject();",
    "        for (auto it = properties.begin(); it != properties.end(); ++it) if (object.contains(it.key()) && (!it.value().isObject() || !matchesSchema(it.value().toObject(), object.value(it.key()), depth + 1))) return false;",
    "        for (auto it = object.begin(); it != object.end(); ++it) if (!properties.contains(it.key())) { const auto additional = rule.value(QStringLiteral(\"additionalProperties\")); if (additional.isBool() && !additional.toBool()) return false; if (additional.isObject() && !matchesSchema(additional.toObject(), it.value(), depth + 1)) return false; }",
    "    }",
    "    return true;",
    "}",
    "bool decodeSafeInteger(const QJsonValue &value, qint64 minimum, qint64 maximum, qint64 *output) {",
    "    if (!value.isDouble()) return false;",
    "    const double number = value.toDouble();",
    "    if (!std::isfinite(number) || number < static_cast<double>(minimum) || number > static_cast<double>(maximum) || std::floor(number) != number) return false;",
    "    *output = static_cast<qint64>(number); return true;",
    "}",
    "",
  ];
  for (const declaration of declarations.values()) out.push(`bool decodeValue(const QJsonValue &, ${declaration.name} *, QString *);`, `QJsonValue encodeValue(const ${declaration.name} &);`);
  out.push(
    "",
    "template <typename T> QJsonArray encodeArray(const QList<T> &values) { QJsonArray out; for (const auto &value : values) out.append(encodeValue(value)); return out; }",
    "template <> QJsonArray encodeArray<QString>(const QList<QString> &values) { QJsonArray out; for (const auto &value : values) out.append(value); return out; }",
    "template <> QJsonArray encodeArray<QJsonValue>(const QList<QJsonValue> &values) { QJsonArray out; for (const auto &value : values) out.append(value); return out; }",
    "template <typename T> QJsonObject encodeMap(const QMap<QString, T> &values) { QJsonObject out; for (auto it = values.begin(); it != values.end(); ++it) out.insert(it.key(), encodeValue(it.value())); return out; }",
    "",
  );
  for (const declaration of declarations.values()) {
    if (declaration.kind === "json") {
      out.push(`bool decodeValue(const QJsonValue &input, ${declaration.name} *output, QString *) { output->value = input; return true; }`, `QJsonValue encodeValue(const ${declaration.name} &value) { return value.value; }`, "");
    } else if (declaration.kind === "alias") {
      out.push(`bool decodeValue(const QJsonValue &input, ${declaration.name} *output, QString *error) {`);
      out.push(...cppDecode(declaration.type, "input", "output->value"));
      out.push("    return true;", "}", `QJsonValue encodeValue(const ${declaration.name} &value) { return ${cppEncode(declaration.type, "value.value")}; }`, "");
    } else if (declaration.kind === "enum") {
      out.push(`bool decodeValue(const QJsonValue &input, ${declaration.name} *output, QString *error) {`, "    if (!input.isString()) return fail(error, QStringLiteral(\"expected enum string\"));", "    const auto text = input.toString();");
      declaration.values.forEach((value, index) => out.push(`    ${index ? "else " : ""}if (text == QStringLiteral(${JSON.stringify(value)})) *output = ${declaration.name}::${enumVariant(value)};`));
      out.push("    else return fail(error, QStringLiteral(\"unknown enum value\"));", "    return true;", "}", `QJsonValue encodeValue(const ${declaration.name} &value) {`, "    switch (value) {");
      declaration.values.forEach((value) => out.push(`    case ${declaration.name}::${enumVariant(value)}: return QStringLiteral(${JSON.stringify(value)});`));
      out.push("    }", "    return QJsonValue();", "}", "");
    } else if (declaration.kind === "union") {
      out.push(`bool decodeValue(const QJsonValue &input, ${declaration.name} *output, QString *error) {`);
      declaration.variants.forEach((variant) => {
        out.push(`    { ${cppType(variant.type)} decoded; QString ignored; QString *attemptError = &ignored;`, "        const bool matched = [&]() -> bool {");
        const decode = cppDecode(variant.type, "input", "decoded", "            ")
          .map((line) => line.replaceAll("error", "attemptError"));
        out.push(...decode, "            return true;", "        }();", `        if (matched) { output->value = std::move(decoded); return true; }`, "    }");
      });
      out.push("    return fail(error, QStringLiteral(\"value does not match generated union\"));", "}", `QJsonValue encodeValue(const ${declaration.name} &value) { return std::visit([](const auto &entry) { return encodeValue(entry); }, value.value); }`, "");
    } else if (declaration.kind === "object") {
      const allowed = declaration.fields.map((field) => `QStringLiteral(${JSON.stringify(field.jsonName)})`).join(", ");
      out.push(`bool decodeValue(const QJsonValue &input, ${declaration.name} *output, QString *error) {`, "    if (!input.isObject()) return fail(error, QStringLiteral(\"expected object\"));", "    const auto object = input.toObject();", `    const QSet<QString> allowed{${allowed}};`, "    for (auto it = object.begin(); it != object.end(); ++it) if (!allowed.contains(it.key())) return fail(error, QStringLiteral(\"unknown object field\"));");
      for (const field of declaration.fields) {
        if (field.required) {
          out.push(`    if (!object.contains(QStringLiteral(${JSON.stringify(field.jsonName)}))) return fail(error, QStringLiteral(\"missing required field\"));`);
          out.push(...cppDecode(field.type, `object.value(QStringLiteral(${JSON.stringify(field.jsonName)}))`, `output->${field.cppFieldName}`));
        } else {
          out.push(`    if (object.contains(QStringLiteral(${JSON.stringify(field.jsonName)}))) {`, `        ${cppType(field.type)} decoded;`);
          out.push(...cppDecode(field.type, `object.value(QStringLiteral(${JSON.stringify(field.jsonName)}))`, "decoded", "        "));
          out.push(`        output->${field.cppFieldName} = std::move(decoded);`, `    } else output->${field.cppFieldName}.reset();`);
        }
      }
      out.push("    return true;", "}", `QJsonValue encodeValue(const ${declaration.name} &value) {`, "    QJsonObject output;");
      for (const field of declaration.fields) {
        if (field.required) out.push(`    output.insert(QStringLiteral(${JSON.stringify(field.jsonName)}), ${cppEncode(field.type, `value.${field.cppFieldName}`)});`);
        else out.push(`    if (value.${field.cppFieldName}.has_value()) output.insert(QStringLiteral(${JSON.stringify(field.jsonName)}), ${cppEncode(field.type, `*value.${field.cppFieldName}`)});`);
      }
      out.push("    return output;", "}", "");
    }
  }
  out.push(
    "QByteArray quoteJsonString(const QString &value) { QByteArray encoded = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact); return encoded.mid(1, encoded.size() - 2); }",
    "QByteArray canonicalJson(const QJsonValue &value) {",
    "    if (value.isNull() || value.isUndefined()) return \"null\";",
    "    if (value.isBool()) return value.toBool() ? \"true\" : \"false\";",
    "    if (value.isString()) return quoteJsonString(value.toString());",
    "    if (value.isDouble()) return QByteArray::number(value.toDouble(), 'f', 0);",
    "    if (value.isArray()) { QByteArray out = \"[\"; bool first = true; for (const auto &entry : value.toArray()) { if (!first) out += ','; first = false; out += canonicalJson(entry); } return out + ']'; }",
    "    QByteArray out = \"{\"; bool first = true; const auto object = value.toObject(); for (auto it = object.begin(); it != object.end(); ++it) { if (!first) out += ','; first = false; out += quoteJsonString(it.key()); out += ':'; out += canonicalJson(it.value()); } return out + '}';",
    "}",
    "} // namespace",
    "",
    "bool validateCoreDefinition(const QString &name, const QJsonValue &value, QString *error) {",
    "    const QJsonValue definition = coreSchema().value(QStringLiteral(\"$defs\")).toObject().value(name);",
    "    if (!definition.isObject()) return fail(error, QStringLiteral(\"unknown core definition\"));",
    "    if (!hasOnlyUnicodeScalars(value)) return fail(error, QStringLiteral(\"value contains invalid Unicode\"));",
    "    if (name == QStringLiteral(\"itemDataValue\") && !validateItemDataValue(value)) return fail(error, QStringLiteral(\"value exceeds generated ItemData limits\"));",
    "    if (!matchesSchema(definition.toObject(), value)) return fail(error, QStringLiteral(\"value does not match core schema\"));",
    "    return true;",
    "}",
    "",
    "bool decodeCoreFixtureCatalog(const QJsonValue &value, CoreFixtureCatalog *output, QString *error) {",
    "    CoreFixtureCatalog decoded;",
    "    if (!decodeValue(value, &decoded, error)) return false;",
    "    const auto object = value.toObject();",
  );
  for (const entry of fixtureMap.entries) {
    if (entry.array) {
      out.push(`    for (const auto &item : object.value(QStringLiteral(${JSON.stringify(entry.key)})).toArray()) if (!validateCoreDefinition(QStringLiteral(${JSON.stringify(entry.definition)}), item, error)) return false;`);
    } else {
      out.push(`    if (!validateCoreDefinition(QStringLiteral(${JSON.stringify(entry.definition)}), object.value(QStringLiteral(${JSON.stringify(entry.key)})), error)) return false;`);
    }
  }
  out.push(
    "    *output = std::move(decoded);",
    "    return true;",
    "}",
    "QJsonValue encodeCoreFixtureCatalog(const CoreFixtureCatalog &value) { return encodeValue(value); }",
    "QByteArray canonicalCoreFixtureCatalog(const CoreFixtureCatalog &value) { CoreFixtureCatalog verified; const QJsonValue encoded = encodeValue(value); if (!decodeCoreFixtureCatalog(encoded, &verified)) return {}; return canonicalJson(encoded); }",
    "",
    "} // namespace aegisy::aap::generated",
    "",
  );
  return `${out.join("\n").replace(/\n+$/u, "")}\n`;
}

const outputs = new Map([
  [resolve(packageRoot, "../crates/aegisy-aap/src/generated_core.rs"), renderRust()],
  [resolve(packageRoot, "generated/typescript/core_types.d.ts"), renderTypeScriptDeclarations()],
  [resolve(packageRoot, "generated/typescript/core_types.mjs"), renderTypeScriptRuntime()],
  [resolve(packageRoot, "generated/cpp/aap_core_types_generated.h"), renderCppHeader()],
  [resolve(packageRoot, "generated/cpp/aap_core_types_generated.cpp"), renderCppSource()],
]);

let stale = false;
for (const [outputPath, content] of outputs) {
  if (checkOnly) {
    let existing = "";
    try { existing = readFileSync(outputPath, "utf8"); } catch (_) { /* missing is stale */ }
    if (existing !== content) {
      const relativeOutput = relative(packageRoot, outputPath).replaceAll("\\", "/");
      console.error(`generated output is stale: ${relativeOutput}`);
      stale = true;
    }
  } else {
    mkdirSync(dirname(outputPath), { recursive: true });
    writeFileSync(outputPath, content);
  }
}
if (stale) process.exitCode = 1;
