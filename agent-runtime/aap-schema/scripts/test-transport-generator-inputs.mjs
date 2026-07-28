import { spawnSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const packageRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const generator = resolve(packageRoot, "scripts/generate-transport-types.mjs");
const schema = JSON.parse(readFileSync(resolve(packageRoot, "stable/v0.1/aap.schema.json"), "utf8"));
const methodRegistry = JSON.parse(readFileSync(resolve(packageRoot, "fixtures/aap-transport-methods.json"), "utf8"));
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

const expectRegistryRejected = (name, candidate, expectedMessage, validateSemantics = true) => {
  const registryPath = resolve(temporaryRoot, `${name}.registry.json`);
  writeFileSync(registryPath, `${JSON.stringify(candidate)}\n`);
  const argumentsList = [generator, "--check"];
  if (validateSemantics) argumentsList.push("--validate-registry-candidate");
  argumentsList.push("--method-registry", registryPath);
  const result = spawnSync(process.execPath, argumentsList, {
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

  const freshnessDrift = clone(schema);
  freshnessDrift.$defs.runtimeHeartbeatResult.$comment =
    "Semantically permitted documentation drift must still regenerate every language output.";
  expectRejected("generated-freshness-drift", freshnessDrift, "generated output is stale");

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

  const requestMethodDrift = clone(schema);
  requestMethodDrift.$defs.runtimeHeartbeatRequest.properties.method.const = "future/heartbeat";
  expectRejected(
    "request-method-drift",
    requestMethodDrift,
    "runtime/heartbeat request definition does not bind its method and params",
  );

  const requestParamsDrift = clone(schema);
  requestParamsDrift.$defs.runtimeHeartbeatRequest.properties.params.$ref = "#/$defs/initializeParams";
  expectRejected(
    "request-params-drift",
    requestParamsDrift,
    "runtime/heartbeat request definition does not bind its method and params",
  );

  const successEnvelopeDrift = clone(schema);
  successEnvelopeDrift.$defs.runtimeHeartbeatSuccessResponse.required = ["jsonrpc", "id"];
  expectRejected(
    "success-envelope-drift",
    successEnvelopeDrift,
    "runtime/heartbeat success response definition is not a strict success envelope",
  );

  const successResultDrift = clone(schema);
  successResultDrift.$defs.runtimeHeartbeatSuccessResponse.properties.result.$ref =
    "#/$defs/initializeResult";
  expectRejected(
    "success-result-drift",
    successResultDrift,
    "runtime/heartbeat success response definition is not a strict success envelope",
  );

  const notificationMethodDrift = clone(schema);
  notificationMethodDrift.$defs.timelineSubscriptionEventNotification.properties.method.const =
    "future/subscription-event";
  expectRejected(
    "notification-method-drift",
    notificationMethodDrift,
    "timeline/subscription-event notification definition does not bind its method and params",
  );

  const typedErrorEnvelopeDrift = clone(schema);
  typedErrorEnvelopeDrift.$defs.timelineSyncRetentionGapErrorResponse.properties.error.required =
    ["code", "message"];
  expectRejected(
    "typed-error-envelope-drift",
    typedErrorEnvelopeDrift,
    "timeline/sync typed error definition is not a strict error envelope",
  );

  const typedErrorOpenObject = clone(schema);
  typedErrorOpenObject.$defs.timelineSyncRetentionGapErrorResponse.properties.error.additionalProperties = true;
  expectRejected(
    "typed-error-open-object",
    typedErrorOpenObject,
    "timeline/sync typed error definition is not a strict error envelope",
  );

  const typedErrorCodeDrift = clone(schema);
  typedErrorCodeDrift.$defs.timelineSyncRetentionGapErrorResponse.properties.error.properties.code =
    { type: "string" };
  expectRejected(
    "typed-error-code-drift",
    typedErrorCodeDrift,
    "timeline/sync typed error definition is not a strict error envelope",
  );

  const unclassifiedRootCondition = clone(schema);
  unclassifiedRootCondition.allOf.push({
    if: { required: ["result"] },
    then: { properties: { id: { $ref: "#/$defs/jsonRpcRequestId" } } },
  });
  expectRejected(
    "unclassified-root-condition",
    unclassifiedRootCondition,
    "is not covered by method or typed-error dispatch",
  );

  const optionalDiscriminator = clone(schema);
  optionalDiscriminator.$defs.initializeIncompatibleData.required =
    optionalDiscriminator.$defs.initializeIncompatibleData.required.filter(
      (field) => field !== "schema_version",
    );
  expectRejected(
    "optional-error-discriminator",
    optionalDiscriminator,
    "has no required unique schema_version discriminator",
  );

  const conflictingDiscriminator = clone(schema);
  conflictingDiscriminator.$defs.initializeIncompatibleData.allOf = [{
    required: ["schema_version"],
    properties: { schema_version: { const: "conflicting-error/0.1" } },
  }];
  expectRejected(
    "conflicting-error-discriminator",
    conflictingDiscriminator,
    "has no required unique schema_version discriminator",
  );

  const requestSwap = clone(methodRegistry);
  [requestSwap.methods[1].request_definition, requestSwap.methods[4].request_definition] =
    [requestSwap.methods[4].request_definition, requestSwap.methods[1].request_definition];
  expectRegistryRejected("request-definition-swap", requestSwap,
    "initialize request definition does not bind its method and params");

  const successSwap = clone(methodRegistry);
  [successSwap.methods[1].success_response_definition, successSwap.methods[4].success_response_definition] =
    [successSwap.methods[4].success_response_definition, successSwap.methods[1].success_response_definition];
  expectRegistryRejected("success-definition-swap", successSwap,
    "initialize success response definition is not a strict success envelope");

  const notificationSwap = clone(methodRegistry);
  [notificationSwap.methods[9].notification_definition, notificationSwap.methods[10].notification_definition] =
    [notificationSwap.methods[10].notification_definition, notificationSwap.methods[9].notification_definition];
  expectRegistryRejected("notification-definition-swap", notificationSwap,
    "timeline/subscription-event notification definition does not bind its method and params");

  const errorSwap = clone(methodRegistry);
  [errorSwap.methods[1].error_response_definitions, errorSwap.methods[13].error_response_definitions] =
    [errorSwap.methods[13].error_response_definitions, errorSwap.methods[1].error_response_definitions];
  expectRegistryRejected("error-definition-swap", errorSwap,
    "transport method registry differs from reviewed golden", false);

  const duplicateErrors = clone(methodRegistry);
  duplicateErrors.methods[7].error_response_definitions.push(
    duplicateErrors.methods[7].error_response_definitions[0],
  );
  expectRegistryRejected("duplicate-error-definitions", duplicateErrors,
    "error response definitions must be known, unique, and sorted");

  const typedStageDrift = clone(methodRegistry);
  typedStageDrift.methods[7].typed_error_stage = "activate";
  expectRegistryRejected("typed-error-stage-drift", typedStageDrift,
    "timeline/subscribe typed error stage is invalid");

  const methodKindDrift = clone(methodRegistry);
  methodKindDrift.methods[0].kind = "request";
  expectRegistryRejected("method-kind-drift", methodKindDrift,
    "transport method registry dispatch mismatch for event");

  const unknownSuccessDefinition = clone(methodRegistry);
  unknownSuccessDefinition.methods[4].success_response_definition = "futureSuccessResponse";
  expectRegistryRejected("unknown-success-definition", unknownSuccessDefinition,
    "runtime/heartbeat has unknown success_response_definition");
} finally {
  rmSync(temporaryRoot, { recursive: true, force: true });
}
