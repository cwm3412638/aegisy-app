import { spawnSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const generator = resolve(packageRoot, "scripts/generate-transport-types.mjs");
const schema = JSON.parse(readFileSync(resolve(packageRoot, "stable/v0.1/aap.schema.json"), "utf8"));
const temporaryRoot = mkdtempSync(resolve(tmpdir(), "aegisy-aap-transport-generator-"));
const clone = (value) => JSON.parse(JSON.stringify(value));

const expectRejected = (name, candidate, expectedMessage) => {
  const schemaPath = resolve(temporaryRoot, `${name}.schema.json`);
  writeFileSync(schemaPath, `${JSON.stringify(candidate)}\n`);
  const result = spawnSync(process.execPath, [generator, "--check", "--schema", schemaPath], {
    encoding: "utf8",
  });
  const diagnostic = `${result.stdout}${result.stderr}`;
  if (result.status === 0 || !diagnostic.includes(expectedMessage)) {
    throw new Error(`${name} did not fail with ${expectedMessage}: ${diagnostic}`);
  }
};

try {
  const dialect = clone(schema);
  dialect.$schema = "https://json-schema.org/draft/2019-09/schema";
  expectRejected("dialect", dialect, "requires the stable AAP 0.1 Draft 2020-12 schema");

  const identity = clone(schema);
  identity.$id = "https://aegisy.cc/schemas/aap/stable/v0.2/aap.schema.json";
  expectRejected("identity", identity, "requires the stable AAP 0.1 Draft 2020-12 schema");

  const unknownKeyword = clone(schema);
  unknownKeyword.$defs.initializeParams.properties.client.format = "uuid";
  expectRejected("unknown-keyword", unknownKeyword, "unsupported keyword format");

  const unknownReference = clone(schema);
  unknownReference.$defs.initializeParams.properties.client.$ref = "#/$defs/missingType";
  expectRejected("unknown-reference", unknownReference, "uses an unsupported reference");

  const unsupportedType = clone(schema);
  unsupportedType.$defs.safePositiveInteger.type = "number";
  expectRejected("unsupported-type", unsupportedType, "uses unsupported type number");

  const unsafeBound = clone(schema);
  unsafeBound.$defs.safePositiveInteger.maximum = 9_007_199_254_740_992;
  expectRejected("unsafe-bound", unsafeBound, "must be a JSON-safe integer");

  const typeCollision = clone(schema);
  typeCollision.$defs["safe-positive-integer"] = clone(typeCollision.$defs.safePositiveInteger);
  expectRejected("type-collision", typeCollision, "definition name collision");

  const fieldCollision = clone(schema);
  fieldCollision.$defs.initializeParams.properties["client-name"] = { type: "string" };
  fieldCollision.$defs.initializeParams.properties.client_name = { type: "string" };
  expectRejected("field-collision", fieldCollision, "collision in InitializeParams");

  const enumCollision = clone(schema);
  enumCollision.$defs.generatedEnumCollision = {
    type: "string",
    enum: ["foo-bar", "foo_bar"],
  };
  expectRejected("enum-collision", enumCollision, "enum collision in GeneratedEnumCollision");

  const dependencyCycle = clone(schema);
  dependencyCycle.$defs.generatedCycleA = { $ref: "#/$defs/generatedCycleB" };
  dependencyCycle.$defs.generatedCycleB = { $ref: "#/$defs/generatedCycleA" };
  expectRejected("dependency-cycle", dependencyCycle, "by-value dependency cycle");
} finally {
  rmSync(temporaryRoot, { recursive: true, force: true });
}
