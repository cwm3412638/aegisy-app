import { spawnSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const generator = resolve(packageRoot, "scripts/generate-core-types.mjs");
const schema = JSON.parse(readFileSync(resolve(packageRoot, "stable/v0.1/core.schema.json"), "utf8"));
const fixtureMap = JSON.parse(readFileSync(resolve(packageRoot, "fixtures/aap-core-domains.fixture-map.json"), "utf8"));
const fixtureCatalog = JSON.parse(readFileSync(resolve(packageRoot, "fixtures/aap-core-domains.json"), "utf8"));
const temporaryRoot = mkdtempSync(resolve(tmpdir(), "aegisy-aap-generator-"));

const clone = (value) => JSON.parse(JSON.stringify(value));

function expectRejected(name, map, catalog, expectedMessage) {
  const mapPath = resolve(temporaryRoot, `${name}.map.json`);
  const catalogPath = resolve(temporaryRoot, `${name}.catalog.json`);
  writeFileSync(mapPath, `${JSON.stringify(map)}\n`);
  writeFileSync(catalogPath, `${JSON.stringify(catalog)}\n`);
  const result = spawnSync(
    process.execPath,
    [generator, "--check", "--fixture-map", mapPath, "--fixture-catalog", catalogPath],
    { encoding: "utf8" },
  );
  const diagnostic = `${result.stdout}${result.stderr}`;
  if (result.status === 0 || !diagnostic.includes(expectedMessage)) {
    throw new Error(`${name} did not fail with ${expectedMessage}: ${diagnostic}`);
  }
}

function expectSchemaRejected(name, candidate, expectedMessage) {
  const schemaPath = resolve(temporaryRoot, `${name}.schema.json`);
  writeFileSync(schemaPath, `${JSON.stringify(candidate)}\n`);
  const result = spawnSync(
    process.execPath,
    [generator, "--check", "--schema", schemaPath],
    { encoding: "utf8" },
  );
  const diagnostic = `${result.stdout}${result.stderr}`;
  if (result.status === 0 || !diagnostic.includes(expectedMessage)) {
    throw new Error(`${name} did not fail with ${expectedMessage}: ${diagnostic}`);
  }
}

try {
  const unknownTopLevel = clone(fixtureMap);
  unknownTopLevel.unexpected = true;
  expectRejected("unknown-top-level", unknownTopLevel, fixtureCatalog, "unknown field unexpected");

  const unknownEntryField = clone(fixtureMap);
  unknownEntryField.entries[0].unexpected = true;
  expectRejected("unknown-entry-field", unknownEntryField, fixtureCatalog, "unknown field unexpected");

  const duplicateKey = clone(fixtureMap);
  duplicateKey.entries.push(clone(duplicateKey.entries[0]));
  expectRejected("duplicate-key", duplicateKey, fixtureCatalog, "duplicate fixture map key");

  const nonBooleanArray = clone(fixtureMap);
  nonBooleanArray.entries[0].array = "false";
  expectRejected("non-boolean-array", nonBooleanArray, fixtureCatalog, "array must be boolean");

  const missingCatalogKey = clone(fixtureCatalog);
  delete missingCatalogKey.project;
  expectRejected("missing-catalog-key", fixtureMap, missingCatalogKey, "fixture map/catalog keys differ");

  const unexpectedCatalogKey = clone(fixtureCatalog);
  unexpectedCatalogKey.unexpected = null;
  expectRejected("unexpected-catalog-key", fixtureMap, unexpectedCatalogKey, "fixture map/catalog keys differ");

  const invalidGoldenDigest = clone(fixtureMap);
  invalidGoldenDigest.canonical_sha256 = "invalid";
  expectRejected("invalid-golden-digest", invalidGoldenDigest, fixtureCatalog, "lowercase SHA-256 digest");

  const unknownSchemaKeyword = clone(schema);
  unknownSchemaKeyword.$defs.project.properties.id.format = "uuid";
  expectSchemaRejected("unknown-schema-keyword", unknownSchemaKeyword, "unsupported schema keyword format");

  const unsupportedSchemaType = clone(schema);
  unsupportedSchemaType.$defs.safeNonNegativeInteger.type = "number";
  expectSchemaRejected("unsupported-schema-type", unsupportedSchemaType, ".type is unsupported");

  const unboundedInteger = clone(schema);
  delete unboundedInteger.$defs.safePositiveInteger.maximum;
  expectSchemaRejected("unbounded-integer", unboundedInteger, "integer schemas must declare JSON-safe bounds");

  const referenceSibling = clone(schema);
  referenceSibling.$defs.project.properties.id.maxLength = 128;
  expectSchemaRejected("reference-semantic-sibling", referenceSibling, "combines $ref with unsupported semantic siblings");

  const generatedTypeCollision = clone(schema);
  generatedTypeCollision.$defs["safe-positive-integer"] = clone(
    generatedTypeCollision.$defs.safePositiveInteger,
  );
  expectSchemaRejected(
    "generated-type-name-collision",
    generatedTypeCollision,
    "generated type name collision",
  );

  const generatedFieldCollision = clone(schema);
  generatedFieldCollision.$defs.project.properties["display-name"] = {
    $ref: "#/$defs/boundedLabel",
  };
  generatedFieldCollision.$defs.project.properties.display_name = {
    $ref: "#/$defs/boundedLabel",
  };
  expectSchemaRejected(
    "generated-field-name-collision",
    generatedFieldCollision,
    "generated fieldName collision",
  );

  const generatedEnumCollision = clone(schema);
  generatedEnumCollision.$defs.generatedEnumCollision = {
    type: "string",
    enum: ["foo-bar", "foo_bar"],
  };
  expectSchemaRejected(
    "generated-enum-variant-collision",
    generatedEnumCollision,
    "generated enum variant collision",
  );

  const openObject = clone(schema);
  delete openObject.$defs.project.additionalProperties;
  expectSchemaRejected(
    "open-object-with-properties",
    openObject,
    "object schemas with properties must set additionalProperties to false",
  );
} finally {
  rmSync(temporaryRoot, { recursive: true, force: true });
}
