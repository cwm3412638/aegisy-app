import { createHash } from "node:crypto";
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, relative, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
let schemaPath = resolve(packageRoot, "stable/v0.1/aap.schema.json");
let checkOnly = false;
for (let index = 2; index < process.argv.length; index += 1) {
  const argument = process.argv[index];
  if (argument === "--check") checkOnly = true;
  else if (argument === "--schema") {
    const value = process.argv[++index];
    if (!value) throw new Error("--schema requires a path");
    schemaPath = resolve(value);
  } else throw new Error(`unsupported generator argument ${argument}`);
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
  const out = [
    "// @generated by agent-runtime/aap-schema/scripts/generate-transport-types.mjs; do not edit.",
    "use serde::{Deserialize, Serialize};",
    "use serde_json::Value;",
    "",
    `pub const TRANSPORT_SCHEMA_ID: &str = ${JSON.stringify(expectedSchemaId)};`,
    `pub const TRANSPORT_SCHEMA_SHA256: &str = ${JSON.stringify(schemaSha256)};`,
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
    "fn validation_error() -> serde_json::Error {",
    "    serde_json::Error::io(std::io::Error::new(std::io::ErrorKind::InvalidData, \"value does not match generated transport schema\"))",
    "}",
    "",
    "fn validator_for(rule: Value) -> Result<jsonschema::Validator, serde_json::Error> {",
    "    let root: Value = serde_json::from_str(TRANSPORT_SCHEMA_JSON)?;",
    "    let document = serde_json::json!({",
    "        \"$schema\": root.get(\"$schema\").cloned().unwrap_or(Value::String(\"https://json-schema.org/draft/2020-12/schema\".to_owned())),",
    "        \"$defs\": root.get(\"$defs\").cloned().ok_or_else(validation_error)?,",
    "        \"allOf\": [rule],",
    "    });",
    "    jsonschema::validator_for(&document).map_err(|_| validation_error())",
    "}",
    "",
    "pub fn validate_transport_definition(name: &str, value: &Value) -> Result<(), serde_json::Error> {",
    "    let root: Value = serde_json::from_str(TRANSPORT_SCHEMA_JSON)?;",
    "    let rule = root.get(\"$defs\").and_then(Value::as_object).and_then(|defs| defs.get(name)).cloned().ok_or_else(validation_error)?;",
    "    if validator_for(rule)?.is_valid(value) { Ok(()) } else { Err(validation_error()) }",
    "}",
    "",
    "pub fn validate_transport_message(value: &Value) -> Result<(), serde_json::Error> {",
    "    let root: Value = serde_json::from_str(TRANSPORT_SCHEMA_JSON)?;",
    "    if jsonschema::validator_for(&root).is_ok_and(|validator| validator.is_valid(value)) { Ok(()) } else { Err(validation_error()) }",
    "}",
    "",
    "pub fn decode_transport_definition_raw(name: &str, bytes: &[u8]) -> Result<Value, serde_json::Error> {",
    "    let value = crate::transport_json::parse_transport_json(bytes).map_err(|_| validation_error())?;",
    "    validate_transport_definition(name, &value)?;",
    "    Ok(value)",
    "}",
    "",
    "pub fn decode_transport_message_raw(bytes: &[u8]) -> Result<TransportMessage, serde_json::Error> {",
    "    let value = crate::transport_json::parse_transport_json(bytes).map_err(|_| validation_error())?;",
    "    validate_transport_message(&value)?;",
    "    Ok(TransportMessage(value))",
    "}",
    "",
  );
  const rendered = out.join("\n").replace(
    /^(pub (?:const|type|enum|struct|fn)|fn )/gm,
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
