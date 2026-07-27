import { createHash } from "node:crypto";

export const MAX_RAW_JSON_BYTES = 4 * 1024 * 1024;
export const RAW_JSON_MAX_DEPTH = 128;
export const RAW_JSON_MAX_NODES = 65_536;

export class TransportFrameError extends Error {}
export class TransportJsonParseError extends SyntaxError {}
export class TransportImplementationLimitError extends Error {}
export class TransportSchemaValidationError extends TypeError {}

const SUPPORTED_SCHEMA_KEYWORDS = new Set([
  "$comment", "$defs", "$id", "$ref", "$schema", "additionalProperties",
  "allOf", "anyOf", "const", "else", "enum", "if", "items", "maximum",
  "maxItems", "maxLength", "maxProperties", "minimum", "minItems",
  "minLength", "not", "oneOf", "pattern", "properties", "propertyNames",
  "required", "then", "title", "type", "uniqueItems",
]);

const RAW_JSON_NUMBERS = new WeakSet();

const isRawJsonNumber = (value) => value !== null && typeof value === "object" &&
  RAW_JSON_NUMBERS.has(value);
export const isTransportJsonNumber = (value) => isRawJsonNumber(value);
const isObject = (value) => value !== null && typeof value === "object" &&
  !Array.isArray(value) && !isRawJsonNumber(value);

const unsignedDecimal = (digits) => {
  const normalized = digits.replace(/^0+/, "");
  return normalized === "" ? "0" : normalized;
};

const signedDecimal = (negative, digits) => {
  const normalized = unsignedDecimal(digits);
  return { negative: normalized !== "0" && negative, digits: normalized };
};

const compareUnsignedDecimal = (left, right) => left.length !== right.length
  ? Math.sign(left.length - right.length)
  : left < right ? -1 : left > right ? 1 : 0;

const addUnsignedDecimal = (left, right) => {
  let carry = 0;
  let result = "";
  for (let leftIndex = left.length - 1, rightIndex = right.length - 1;
       leftIndex >= 0 || rightIndex >= 0 || carry !== 0;
       leftIndex -= 1, rightIndex -= 1) {
    const sum = (leftIndex >= 0 ? left.charCodeAt(leftIndex) - 48 : 0) +
      (rightIndex >= 0 ? right.charCodeAt(rightIndex) - 48 : 0) + carry;
    result = String(sum % 10) + result;
    carry = Math.floor(sum / 10);
  }
  return unsignedDecimal(result);
};

const subtractUnsignedDecimal = (left, right) => {
  let borrow = 0;
  let result = "";
  for (let leftIndex = left.length - 1, rightIndex = right.length - 1;
       leftIndex >= 0;
       leftIndex -= 1, rightIndex -= 1) {
    let digit = left.charCodeAt(leftIndex) - 48 - borrow -
      (rightIndex >= 0 ? right.charCodeAt(rightIndex) - 48 : 0);
    borrow = digit < 0 ? 1 : 0;
    if (borrow) digit += 10;
    result = String(digit) + result;
  }
  return unsignedDecimal(result);
};

const addSignedDecimal = (left, right) => {
  if (left.negative === right.negative) {
    return signedDecimal(left.negative, addUnsignedDecimal(left.digits, right.digits));
  }
  const comparison = compareUnsignedDecimal(left.digits, right.digits);
  if (comparison === 0) return signedDecimal(false, "0");
  return comparison > 0
    ? signedDecimal(left.negative, subtractUnsignedDecimal(left.digits, right.digits))
    : signedDecimal(right.negative, subtractUnsignedDecimal(right.digits, left.digits));
};

const addSignedSmall = (value, delta) => addSignedDecimal(
  value,
  signedDecimal(delta < 0, String(Math.abs(delta))),
);

const compareSignedDecimal = (left, right) => {
  if (left.negative !== right.negative) return left.negative ? -1 : 1;
  const comparison = compareUnsignedDecimal(left.digits, right.digits);
  return left.negative ? -comparison : comparison;
};

class RawJsonNumber {
  constructor(literal, negative, coefficient, scale) {
    this.literal = literal;
    this.negative = coefficient !== "0" && negative;
    this.coefficient = coefficient;
    this.scale = scale;
    RAW_JSON_NUMBERS.add(this);
    Object.freeze(this.scale);
    Object.freeze(this);
  }

  get lexical() {
    return this.literal;
  }

  get canonical() {
    return canonicalRawNumber(this);
  }

  get integer() {
    return this.isInteger();
  }

  static fromParts(literal, negative, integerDigits, fractionDigits, exponent) {
    let coefficient = unsignedDecimal(`${integerDigits}${fractionDigits}`);
    if (coefficient === "0") return new RawJsonNumber(literal, false, coefficient, signedDecimal(false, "0"));
    let trailingZeros = 0;
    while (coefficient.endsWith("0")) {
      coefficient = coefficient.slice(0, -1);
      trailingZeros += 1;
    }
    const scale = addSignedSmall(exponent, trailingZeros - fractionDigits.length);
    return new RawJsonNumber(literal, negative, coefficient, scale);
  }

  static fromSchemaNumber(value) {
    if (typeof value !== "number" || !Number.isFinite(value)) {
      throw new TypeError("Schema numeric values must be finite JSON numbers");
    }
    return rawJsonNumberFromLiteral(JSON.stringify(value));
  }

  isInteger() {
    return this.coefficient === "0" || !this.scale.negative;
  }
}

const rawJsonNumberFromLiteral = (literal) => {
  let index = 0;
  const negative = literal[index] === "-";
  if (negative) index += 1;
  const integerStart = index;
  while (/[0-9]/.test(literal[index] ?? "")) index += 1;
  const integerDigits = literal.slice(integerStart, index);
  let fractionDigits = "";
  if (literal[index] === ".") {
    index += 1;
    const fractionStart = index;
    while (/[0-9]/.test(literal[index] ?? "")) index += 1;
    fractionDigits = literal.slice(fractionStart, index);
  }
  let exponent = signedDecimal(false, "0");
  if (literal[index] === "e" || literal[index] === "E") {
    index += 1;
    const exponentNegative = literal[index] === "-";
    if (literal[index] === "+" || literal[index] === "-") index += 1;
    exponent = signedDecimal(exponentNegative, literal.slice(index));
  }
  return RawJsonNumber.fromParts(literal, negative, integerDigits, fractionDigits, exponent);
};

const compareRawJsonNumbers = (left, right) => {
  if (left.coefficient === "0" || right.coefficient === "0") {
    if (left.coefficient === right.coefficient) return 0;
    return left.coefficient === "0" ? (right.negative ? 1 : -1) : (left.negative ? -1 : 1);
  }
  if (left.negative !== right.negative) return left.negative ? -1 : 1;
  const leftOrder = addSignedSmall(left.scale, left.coefficient.length);
  const rightOrder = addSignedSmall(right.scale, right.coefficient.length);
  let comparison = compareSignedDecimal(leftOrder, rightOrder);
  if (comparison === 0) {
    const width = Math.max(left.coefficient.length, right.coefficient.length);
    for (let index = 0; index < width && comparison === 0; index += 1) {
      comparison = (left.coefficient.charCodeAt(index) || 48) -
        (right.coefficient.charCodeAt(index) || 48);
    }
  }
  comparison = Math.sign(comparison);
  return left.negative ? -comparison : comparison;
};

const numberValue = (value) => isRawJsonNumber(value)
  ? value
  : typeof value === "number" ? RawJsonNumber.fromSchemaNumber(value) : null;

const deepEqual = (left, right) => {
  const leftNumber = numberValue(left);
  const rightNumber = numberValue(right);
  if (leftNumber || rightNumber) {
    return leftNumber !== null && rightNumber !== null &&
      leftNumber.negative === rightNumber.negative &&
      leftNumber.coefficient === rightNumber.coefficient &&
      compareSignedDecimal(leftNumber.scale, rightNumber.scale) === 0;
  }
  if (Object.is(left, right)) return true;
  if (typeof left !== typeof right || left === null || right === null) return false;
  if (Array.isArray(left)) {
    return Array.isArray(right) && left.length === right.length &&
      left.every((value, index) => deepEqual(value, right[index]));
  }
  if (!isObject(left) || !isObject(right)) return false;
  const leftKeys = Object.keys(left).sort();
  const rightKeys = Object.keys(right).sort();
  return leftKeys.length === rightKeys.length &&
    leftKeys.every((key, index) => key === rightKeys[index] && deepEqual(left[key], right[key]));
};

const scalarLength = (value) => [...value].length;

class RawJsonParser {
  constructor(raw) {
    if (typeof raw !== "string") throw new TypeError("raw JSON must be a string");
    if (Buffer.byteLength(raw, "utf8") > MAX_RAW_JSON_BYTES) {
      throw new TransportFrameError("raw JSON exceeds the 4 MiB transport frame limit");
    }
    this.raw = raw;
    this.index = 0;
    this.depth = 0;
    this.nodes = 0;
  }

  parse() {
    this.skipWhitespace();
    const value = this.parseValue();
    this.skipWhitespace();
    if (this.index !== this.raw.length) throw new TransportJsonParseError("trailing bytes after JSON value");
    return value;
  }

  parseValue() {
    this.nodes += 1;
    if (this.nodes > RAW_JSON_MAX_NODES) throw new TransportImplementationLimitError("raw JSON exceeds the parser node limit");
    const char = this.raw[this.index];
    if (char === "\"") return this.parseString();
    if (char === "{") return this.withDepth(() => this.parseObject());
    if (char === "[") return this.withDepth(() => this.parseArray());
    if (char === "t" && this.consumeLiteral("true")) return true;
    if (char === "f" && this.consumeLiteral("false")) return false;
    if (char === "n" && this.consumeLiteral("null")) return null;
    if (char === "-" || (char >= "0" && char <= "9")) return this.parseNumber();
    throw new TransportJsonParseError(`invalid JSON value at byte ${this.index}`);
  }

  withDepth(callback) {
    this.depth += 1;
    if (this.depth > RAW_JSON_MAX_DEPTH) throw new TransportImplementationLimitError("raw JSON exceeds the parser depth limit");
    try {
      return callback();
    } finally {
      this.depth -= 1;
    }
  }

  parseObject() {
    this.index += 1;
    this.skipWhitespace();
    const result = Object.create(null);
    const keys = new Set();
    if (this.raw[this.index] === "}") {
      this.index += 1;
      return result;
    }
    while (true) {
      if (this.raw[this.index] !== "\"") throw new TransportJsonParseError("object key must be a JSON string");
      const key = this.parseString();
      if (keys.has(key)) throw new TransportJsonParseError(`duplicate object key ${JSON.stringify(key)}`);
      keys.add(key);
      this.skipWhitespace();
      if (this.raw[this.index] !== ":") throw new TransportJsonParseError("object key is missing a colon");
      this.index += 1;
      this.skipWhitespace();
      result[key] = this.parseValue();
      this.skipWhitespace();
      const separator = this.raw[this.index];
      this.index += 1;
      if (separator === "}") return result;
      if (separator !== ",") throw new TransportJsonParseError("object entry is missing a comma");
      this.skipWhitespace();
    }
  }

  parseArray() {
    this.index += 1;
    this.skipWhitespace();
    const result = [];
    if (this.raw[this.index] === "]") {
      this.index += 1;
      return result;
    }
    while (true) {
      result.push(this.parseValue());
      this.skipWhitespace();
      const separator = this.raw[this.index];
      this.index += 1;
      if (separator === "]") return result;
      if (separator !== ",") throw new TransportJsonParseError("array entry is missing a comma");
      this.skipWhitespace();
    }
  }

  parseString() {
    this.index += 1;
    let result = "";
    while (this.index < this.raw.length) {
      const code = this.raw.charCodeAt(this.index);
      if (code === 0x22) {
        this.index += 1;
        return result;
      }
      if (code < 0x20) throw new TransportJsonParseError("unescaped control character in JSON string");
      if (code === 0x5c) {
        this.index += 1;
        result += this.parseEscape();
        continue;
      }
      if (code >= 0xd800 && code <= 0xdbff) {
        const low = this.raw.charCodeAt(this.index + 1);
        if (low < 0xdc00 || low > 0xdfff) throw new TransportJsonParseError("unpaired high surrogate in JSON string");
        result += this.raw.slice(this.index, this.index + 2);
        this.index += 2;
        continue;
      }
      if (code >= 0xdc00 && code <= 0xdfff) throw new TransportJsonParseError("unpaired low surrogate in JSON string");
      result += this.raw[this.index];
      this.index += 1;
    }
    throw new TransportJsonParseError("unterminated JSON string");
  }

  parseEscape() {
    const escape = this.raw[this.index];
    this.index += 1;
    const simple = { "\"": "\"", "\\": "\\", "/": "/", b: "\b", f: "\f", n: "\n", r: "\r", t: "\t" };
    if (Object.hasOwn(simple, escape)) return simple[escape];
    if (escape !== "u") throw new TransportJsonParseError("invalid JSON string escape");
    const high = this.parseHexCodeUnit();
    if (high >= 0xd800 && high <= 0xdbff) {
      if (this.raw.slice(this.index, this.index + 2) !== "\\u") {
        throw new TransportJsonParseError("escaped high surrogate is not followed by an escaped low surrogate");
      }
      this.index += 2;
      const low = this.parseHexCodeUnit();
      if (low < 0xdc00 || low > 0xdfff) throw new TransportJsonParseError("invalid escaped surrogate pair");
      return String.fromCharCode(high, low);
    }
    if (high >= 0xdc00 && high <= 0xdfff) throw new TransportJsonParseError("unpaired escaped low surrogate");
    return String.fromCharCode(high);
  }

  parseHexCodeUnit() {
    const digits = this.raw.slice(this.index, this.index + 4);
    if (!/^[0-9A-Fa-f]{4}$/.test(digits)) throw new TransportJsonParseError("invalid Unicode escape");
    this.index += 4;
    return Number.parseInt(digits, 16);
  }

  parseNumber() {
    const start = this.index;
    if (this.raw[this.index] === "-") this.index += 1;
    if (this.raw[this.index] === "0") {
      this.index += 1;
      if (/[0-9]/.test(this.raw[this.index] ?? "")) throw new TransportJsonParseError("number has a leading zero");
    } else {
      if (!/[1-9]/.test(this.raw[this.index] ?? "")) throw new TransportJsonParseError("invalid number");
      while (/[0-9]/.test(this.raw[this.index] ?? "")) this.index += 1;
    }
    if (this.raw[this.index] === ".") {
      this.index += 1;
      if (!/[0-9]/.test(this.raw[this.index] ?? "")) throw new TransportJsonParseError("fraction is missing digits");
      while (/[0-9]/.test(this.raw[this.index] ?? "")) this.index += 1;
    }
    if (this.raw[this.index] === "e" || this.raw[this.index] === "E") {
      this.index += 1;
      if (this.raw[this.index] === "+" || this.raw[this.index] === "-") this.index += 1;
      if (!/[0-9]/.test(this.raw[this.index] ?? "")) throw new TransportJsonParseError("exponent is missing digits");
      while (/[0-9]/.test(this.raw[this.index] ?? "")) this.index += 1;
    }
    const literal = this.raw.slice(start, this.index);
    return rawJsonNumberFromLiteral(literal);
  }

  consumeLiteral(literal) {
    if (this.raw.slice(this.index, this.index + literal.length) !== literal) return false;
    this.index += literal.length;
    return true;
  }

  skipWhitespace() {
    while (this.raw[this.index] === " " || this.raw[this.index] === "\t" ||
           this.raw[this.index] === "\r" || this.raw[this.index] === "\n") this.index += 1;
  }
}

export const parseTransportJson = (raw) => new RawJsonParser(raw).parse();

const canonicalRawNumber = (value) => {
  if (value.coefficient === "0") return "0";
  const sign = value.negative ? "-" : "";
  if (value.scale.digits === "0") return `${sign}${value.coefficient}`;
  return `${sign}${value.coefficient}e${value.scale.negative ? "-" : ""}${value.scale.digits}`;
};

const compareUtf8 = (left, right) => Buffer.compare(Buffer.from(left, "utf8"), Buffer.from(right, "utf8"));

export const canonicalTransportJson = (value) => {
  const encode = (entry) => {
    if (entry === null) return "null";
    if (typeof entry === "boolean") return entry ? "true" : "false";
    if (typeof entry === "string") return JSON.stringify(entry);
    if (isRawJsonNumber(entry)) return canonicalRawNumber(entry);
    if (Array.isArray(entry)) return `[${entry.map(encode).join(",")}]`;
    if (isObject(entry)) {
      return `{${Object.keys(entry).sort(compareUtf8)
        .map((key) => `${JSON.stringify(key)}:${encode(entry[key])}`).join(",")}}`;
    }
    throw new TypeError("value is not a parsed Transport JSON value");
  };
  const canonical = encode(value);
  if (Buffer.byteLength(canonical, "utf8") > MAX_RAW_JSON_BYTES) {
    throw new TransportFrameError("canonical JSON exceeds the 4 MiB transport frame limit");
  }
  return canonical;
};

const auditSchema = (schema) => {
  if (!isObject(schema) || schema.$schema !== "https://json-schema.org/draft/2020-12/schema") {
    throw new TypeError("transport oracle requires a Draft 2020-12 schema");
  }
  const visit = (node, path, schemaPosition = true) => {
    if (typeof node === "boolean") return;
    if (!isObject(node)) throw new TypeError(`schema node ${path} must be an object or boolean`);
    if (schemaPosition) {
      for (const keyword of Object.keys(node)) {
        if (!SUPPORTED_SCHEMA_KEYWORDS.has(keyword)) {
          throw new TypeError(`unsupported schema keyword ${keyword} at ${path}`);
        }
      }
    }
    for (const keyword of ["allOf", "anyOf", "oneOf"]) {
      if (Object.hasOwn(node, keyword)) node[keyword].forEach((child, index) => visit(child, `${path}/${keyword}/${index}`));
    }
    for (const keyword of ["if", "then", "else", "not", "items", "additionalProperties", "propertyNames"]) {
      if (Object.hasOwn(node, keyword) && (typeof node[keyword] === "boolean" || isObject(node[keyword]))) {
        visit(node[keyword], `${path}/${keyword}`);
      }
    }
    for (const keyword of ["properties", "$defs"]) {
      if (Object.hasOwn(node, keyword)) {
        for (const [name, child] of Object.entries(node[keyword])) visit(child, `${path}/${keyword}/${name}`);
      }
    }
  };
  visit(schema, "#");
};

const valueTypeMatches = (type, value) => {
  if (type === "null") return value === null;
  if (type === "boolean") return typeof value === "boolean";
  if (type === "number") return isRawJsonNumber(value);
  if (type === "integer") return isRawJsonNumber(value) && value.isInteger();
  if (type === "string") return typeof value === "string";
  if (type === "array") return Array.isArray(value);
  if (type === "object") return isObject(value);
  throw new TypeError(`unsupported JSON Schema type ${type}`);
};

export const createTransportOracle = (schema) => {
  auditSchema(schema);
  const definitions = schema.$defs;
  if (!isObject(definitions)) throw new TypeError("transport schema is missing $defs");

  const resolveReference = (reference) => {
    if (reference === "#") return schema;
    const match = /^#\/\$defs\/([A-Za-z][A-Za-z0-9]*)$/.exec(reference);
    if (!match || !Object.hasOwn(definitions, match[1])) {
      throw new TypeError(`unsupported or unknown local reference ${reference}`);
    }
    return definitions[match[1]];
  };

  const validate = (node, value, path = "$", referenceStack = []) => {
    if (node === true) return;
    if (node === false) throw new TransportSchemaValidationError(`${path} is rejected by a false schema`);
    if (Object.hasOwn(node, "$ref")) {
      const reference = node.$ref;
      if (referenceStack.length > 256) throw new TypeError(`${path} exceeds the reference recursion limit`);
      validate(resolveReference(reference), value, path, [...referenceStack, reference]);
    }
    if (Object.hasOwn(node, "type") && !valueTypeMatches(node.type, value)) {
      throw new TransportSchemaValidationError(`${path} must have type ${node.type}`);
    }
    if (Object.hasOwn(node, "const") && !deepEqual(value, node.const)) {
      throw new TransportSchemaValidationError(`${path} does not equal its const value`);
    }
    if (Object.hasOwn(node, "enum") && !node.enum.some((candidate) => deepEqual(value, candidate))) {
      throw new TransportSchemaValidationError(`${path} is outside its enum`);
    }
    if (Object.hasOwn(node, "minimum") && isRawJsonNumber(value) &&
        compareRawJsonNumbers(value, RawJsonNumber.fromSchemaNumber(node.minimum)) < 0) {
      throw new TransportSchemaValidationError(`${path} is below minimum`);
    }
    if (Object.hasOwn(node, "maximum") && isRawJsonNumber(value) &&
        compareRawJsonNumbers(value, RawJsonNumber.fromSchemaNumber(node.maximum)) > 0) {
      throw new TransportSchemaValidationError(`${path} is above maximum`);
    }
    if (typeof value === "string") {
      const length = scalarLength(value);
      if (Object.hasOwn(node, "minLength") && length < node.minLength) throw new TransportSchemaValidationError(`${path} is shorter than minLength`);
      if (Object.hasOwn(node, "maxLength") && length > node.maxLength) throw new TransportSchemaValidationError(`${path} is longer than maxLength`);
      if (Object.hasOwn(node, "pattern") && !new RegExp(node.pattern, "u").test(value)) throw new TransportSchemaValidationError(`${path} does not match pattern`);
    }
    if (Array.isArray(value)) {
      if (Object.hasOwn(node, "minItems") && value.length < node.minItems) throw new TransportSchemaValidationError(`${path} has too few items`);
      if (Object.hasOwn(node, "maxItems") && value.length > node.maxItems) throw new TransportSchemaValidationError(`${path} has too many items`);
      if (node.uniqueItems && value.some((entry, index) => value.slice(0, index).some((prior) => deepEqual(prior, entry)))) {
        throw new TransportSchemaValidationError(`${path} contains duplicate items`);
      }
      if (Object.hasOwn(node, "items")) value.forEach((entry, index) => validate(node.items, entry, `${path}[${index}]`, referenceStack));
    }
    if (isObject(value)) {
      if (Object.hasOwn(node, "minProperties") && Object.keys(value).length < node.minProperties) throw new TransportSchemaValidationError(`${path} has too few properties`);
      if (Object.hasOwn(node, "maxProperties") && Object.keys(value).length > node.maxProperties) throw new TransportSchemaValidationError(`${path} has too many properties`);
      for (const required of node.required ?? []) {
        if (!Object.hasOwn(value, required)) throw new TransportSchemaValidationError(`${path} is missing ${required}`);
      }
      for (const [name, propertySchema] of Object.entries(node.properties ?? {})) {
        if (Object.hasOwn(value, name)) validate(propertySchema, value[name], `${path}.${name}`, referenceStack);
      }
      if (Object.hasOwn(node, "propertyNames")) {
        for (const name of Object.keys(value)) validate(node.propertyNames, name, `${path}{key}`, referenceStack);
      }
      const known = new Set(Object.keys(node.properties ?? {}));
      for (const [name, entry] of Object.entries(value)) {
        if (known.has(name) || !Object.hasOwn(node, "additionalProperties")) continue;
        if (node.additionalProperties === false) throw new TransportSchemaValidationError(`${path} contains unknown property ${name}`);
        if (node.additionalProperties !== true) validate(node.additionalProperties, entry, `${path}.${name}`, referenceStack);
      }
    }
    const branchAccepted = (branch) => {
      try {
        validate(branch, value, path, referenceStack);
        return true;
      } catch (error) {
        if (!(error instanceof TransportSchemaValidationError)) throw error;
        return false;
      }
    };
    if (Object.hasOwn(node, "allOf") && !node.allOf.every(branchAccepted)) throw new TransportSchemaValidationError(`${path} fails allOf`);
    if (Object.hasOwn(node, "anyOf") && !node.anyOf.some(branchAccepted)) throw new TransportSchemaValidationError(`${path} fails anyOf`);
    if (Object.hasOwn(node, "oneOf") && node.oneOf.filter(branchAccepted).length !== 1) throw new TransportSchemaValidationError(`${path} fails oneOf`);
    if (Object.hasOwn(node, "not") && branchAccepted(node.not)) throw new TransportSchemaValidationError(`${path} matches not`);
    if (Object.hasOwn(node, "if")) {
      const selected = branchAccepted(node.if) ? node.then : node.else;
      if (selected !== undefined) validate(selected, value, path, referenceStack);
    }
  };

  const validateDefinitionValue = (definition, value) => {
    if (typeof definition !== "string" || !Object.hasOwn(definitions, definition)) {
      throw new TypeError(`unknown transport definition ${definition}`);
    }
    validate(definitions[definition], value);
  };

  return {
    definitionNames: Object.freeze(Object.keys(definitions).sort()),
    validateDefinitionValue,
    validateDefinitionRaw(definition, raw) {
      const value = parseTransportJson(raw);
      validateDefinitionValue(definition, value);
      return value;
    },
    validateRootRaw(raw) {
      const value = parseTransportJson(raw);
      validate(schema, value);
      return value;
    },
  };
};

export const stableDecisionIdentity = (schemaVersion, decisions) => {
  let bytes = `${schemaVersion}\n`;
  for (const decision of decisions) {
    bytes += `${decision.name}\t${decision.target}\t${decision.accepted ? "accept" : "reject"}\n`;
  }
  return createHash("sha256").update(Buffer.from(bytes, "utf8")).digest("hex");
};
