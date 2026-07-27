use serde_json::{Map, Number, Value};
use std::collections::HashSet;
use std::fmt;

pub const MAX_TRANSPORT_JSON_BYTES: usize = 4 * 1024 * 1024;
pub const MAX_TRANSPORT_JSON_DEPTH: usize = 128;
pub const MAX_TRANSPORT_JSON_NODES: usize = 65_536;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TransportJsonErrorKind {
    Frame,
    Utf8,
    Syntax,
    ImplementationLimit,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TransportJsonError {
    kind: TransportJsonErrorKind,
    offset: usize,
}

impl TransportJsonError {
    pub fn kind(&self) -> TransportJsonErrorKind {
        self.kind
    }

    pub fn offset(&self) -> usize {
        self.offset
    }
}

impl fmt::Display for TransportJsonError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        let description = match self.kind {
            TransportJsonErrorKind::Frame => "JSON exceeds the transport frame limit",
            TransportJsonErrorKind::Utf8 => "JSON is not valid UTF-8",
            TransportJsonErrorKind::Syntax => "JSON syntax is invalid",
            TransportJsonErrorKind::ImplementationLimit => {
                "JSON exceeds a parser implementation limit"
            }
        };
        write!(formatter, "{description} at byte {}", self.offset)
    }
}

impl std::error::Error for TransportJsonError {}

pub fn parse_transport_json(bytes: &[u8]) -> Result<Value, TransportJsonError> {
    if bytes.len() > MAX_TRANSPORT_JSON_BYTES {
        return Err(TransportJsonError {
            kind: TransportJsonErrorKind::Frame,
            offset: MAX_TRANSPORT_JSON_BYTES,
        });
    }
    let source = std::str::from_utf8(bytes).map_err(|error| TransportJsonError {
        kind: TransportJsonErrorKind::Utf8,
        offset: error.valid_up_to(),
    })?;
    Parser {
        source,
        offset: 0,
        depth: 0,
        nodes: 0,
    }
    .parse()
}

pub fn canonical_transport_json(value: &Value) -> Result<Vec<u8>, TransportJsonError> {
    let mut output = Vec::new();
    encode_canonical(value, &mut output);
    if output.len() > MAX_TRANSPORT_JSON_BYTES {
        return Err(TransportJsonError {
            kind: TransportJsonErrorKind::Frame,
            offset: MAX_TRANSPORT_JSON_BYTES,
        });
    }
    Ok(output)
}

fn encode_canonical(value: &Value, output: &mut Vec<u8>) {
    match value {
        Value::Null => output.extend_from_slice(b"null"),
        Value::Bool(true) => output.extend_from_slice(b"true"),
        Value::Bool(false) => output.extend_from_slice(b"false"),
        Value::Number(number) => {
            output.extend_from_slice(canonical_number(number.as_str()).as_bytes());
        }
        Value::String(value) => {
            let encoded =
                serde_json::to_string(value).expect("a JSON string is always serializable");
            output.extend_from_slice(encoded.as_bytes());
        }
        Value::Array(values) => {
            output.push(b'[');
            for (index, value) in values.iter().enumerate() {
                if index != 0 {
                    output.push(b',');
                }
                encode_canonical(value, output);
            }
            output.push(b']');
        }
        Value::Object(values) => {
            output.push(b'{');
            for (index, (key, value)) in values.iter().enumerate() {
                if index != 0 {
                    output.push(b',');
                }
                let encoded =
                    serde_json::to_string(key).expect("a JSON object key is always serializable");
                output.extend_from_slice(encoded.as_bytes());
                output.push(b':');
                encode_canonical(value, output);
            }
            output.push(b'}');
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct SignedDecimal {
    negative: bool,
    digits: String,
}

impl SignedDecimal {
    fn new(negative: bool, digits: &str) -> Self {
        let digits = digits.trim_start_matches('0');
        let digits = if digits.is_empty() { "0" } else { digits };
        Self {
            negative: negative && digits != "0",
            digits: digits.to_owned(),
        }
    }

    fn from_small(value: i64) -> Self {
        Self::new(value < 0, &value.unsigned_abs().to_string())
    }

    fn add(&self, other: &Self) -> Self {
        if self.negative == other.negative {
            return Self::new(
                self.negative,
                &add_unsigned_decimal(&self.digits, &other.digits),
            );
        }
        match compare_unsigned_decimal(&self.digits, &other.digits) {
            std::cmp::Ordering::Equal => Self::new(false, "0"),
            std::cmp::Ordering::Greater => Self::new(
                self.negative,
                &subtract_unsigned_decimal(&self.digits, &other.digits),
            ),
            std::cmp::Ordering::Less => Self::new(
                other.negative,
                &subtract_unsigned_decimal(&other.digits, &self.digits),
            ),
        }
    }
}

fn compare_unsigned_decimal(left: &str, right: &str) -> std::cmp::Ordering {
    left.len().cmp(&right.len()).then_with(|| left.cmp(right))
}

fn add_unsigned_decimal(left: &str, right: &str) -> String {
    let mut result = Vec::with_capacity(left.len().max(right.len()) + 1);
    let mut left = left.bytes().rev();
    let mut right = right.bytes().rev();
    let mut carry = 0u8;
    loop {
        let left = left.next().map(|byte| byte - b'0');
        let right = right.next().map(|byte| byte - b'0');
        if left.is_none() && right.is_none() && carry == 0 {
            break;
        }
        let sum = left.unwrap_or(0) + right.unwrap_or(0) + carry;
        result.push(b'0' + sum % 10);
        carry = sum / 10;
    }
    result.reverse();
    String::from_utf8(result).expect("decimal addition emits ASCII")
}

fn subtract_unsigned_decimal(left: &str, right: &str) -> String {
    debug_assert!(compare_unsigned_decimal(left, right) != std::cmp::Ordering::Less);
    let left = left.as_bytes();
    let right = right.as_bytes();
    let mut result = Vec::with_capacity(left.len());
    let mut borrow = 0i16;
    for offset in 0..left.len() {
        let left_digit = i16::from(left[left.len() - offset - 1] - b'0');
        let right_digit = right
            .len()
            .checked_sub(offset + 1)
            .map_or(0, |index| i16::from(right[index] - b'0'));
        let mut digit = left_digit - borrow - right_digit;
        if digit < 0 {
            digit += 10;
            borrow = 1;
        } else {
            borrow = 0;
        }
        result.push(b'0' + u8::try_from(digit).expect("decimal digit is bounded"));
    }
    while result.len() > 1 && result.last() == Some(&b'0') {
        result.pop();
    }
    result.reverse();
    String::from_utf8(result).expect("decimal subtraction emits ASCII")
}

fn canonical_number(raw: &str) -> String {
    let bytes = raw.as_bytes();
    let mut offset = usize::from(bytes.first() == Some(&b'-'));
    let negative = offset == 1;
    let integer_start = offset;
    while matches!(bytes.get(offset), Some(b'0'..=b'9')) {
        offset += 1;
    }
    let integer = &raw[integer_start..offset];
    let mut fraction = "";
    if bytes.get(offset) == Some(&b'.') {
        offset += 1;
        let start = offset;
        while matches!(bytes.get(offset), Some(b'0'..=b'9')) {
            offset += 1;
        }
        fraction = &raw[start..offset];
    }
    let mut exponent = SignedDecimal::new(false, "0");
    if matches!(bytes.get(offset), Some(b'e' | b'E')) {
        offset += 1;
        let exponent_negative = bytes.get(offset) == Some(&b'-');
        if matches!(bytes.get(offset), Some(b'+' | b'-')) {
            offset += 1;
        }
        exponent = SignedDecimal::new(exponent_negative, &raw[offset..]);
    }

    let mut coefficient = format!("{integer}{fraction}");
    let first_nonzero = coefficient
        .bytes()
        .position(|byte| byte != b'0')
        .unwrap_or(coefficient.len());
    coefficient.drain(..first_nonzero);
    if coefficient.is_empty() {
        return "0".to_owned();
    }
    let trailing_zeros = coefficient
        .bytes()
        .rev()
        .take_while(|byte| *byte == b'0')
        .count();
    coefficient.truncate(coefficient.len() - trailing_zeros);
    let adjustment = i64::try_from(trailing_zeros)
        .and_then(|zeros| i64::try_from(fraction.len()).map(|fraction| zeros - fraction))
        .expect("a transport frame length fits i64");
    let scale = exponent.add(&SignedDecimal::from_small(adjustment));

    let mut canonical = String::new();
    if negative {
        canonical.push('-');
    }
    canonical.push_str(&coefficient);
    if scale.digits != "0" {
        canonical.push('e');
        if scale.negative {
            canonical.push('-');
        }
        canonical.push_str(&scale.digits);
    }
    canonical
}

struct Parser<'a> {
    source: &'a str,
    offset: usize,
    depth: usize,
    nodes: usize,
}

impl Parser<'_> {
    fn parse(mut self) -> Result<Value, TransportJsonError> {
        self.skip_whitespace();
        let value = self.parse_value()?;
        self.skip_whitespace();
        if self.offset != self.source.len() {
            return self.syntax();
        }
        Ok(value)
    }

    fn parse_value(&mut self) -> Result<Value, TransportJsonError> {
        self.nodes = self.nodes.saturating_add(1);
        if self.nodes > MAX_TRANSPORT_JSON_NODES {
            return self.limit();
        }
        match self.peek() {
            Some(b'n') => {
                self.consume_literal(b"null")?;
                Ok(Value::Null)
            }
            Some(b't') => {
                self.consume_literal(b"true")?;
                Ok(Value::Bool(true))
            }
            Some(b'f') => {
                self.consume_literal(b"false")?;
                Ok(Value::Bool(false))
            }
            Some(b'"') => self.parse_string().map(Value::String),
            Some(b'[') => self.with_depth(Self::parse_array),
            Some(b'{') => self.with_depth(Self::parse_object),
            Some(b'-' | b'0'..=b'9') => self.parse_number().map(Value::Number),
            _ => self.syntax(),
        }
    }

    fn with_depth<T>(
        &mut self,
        parse: impl FnOnce(&mut Self) -> Result<T, TransportJsonError>,
    ) -> Result<T, TransportJsonError> {
        self.depth = self.depth.saturating_add(1);
        if self.depth > MAX_TRANSPORT_JSON_DEPTH {
            return self.limit();
        }
        let result = parse(self);
        self.depth -= 1;
        result
    }

    fn parse_array(&mut self) -> Result<Value, TransportJsonError> {
        self.offset += 1;
        self.skip_whitespace();
        let mut values = Vec::new();
        if self.consume_if(b']') {
            return Ok(Value::Array(values));
        }
        loop {
            values.push(self.parse_value()?);
            self.skip_whitespace();
            if self.consume_if(b']') {
                return Ok(Value::Array(values));
            }
            self.expect(b',')?;
            self.skip_whitespace();
        }
    }

    fn parse_object(&mut self) -> Result<Value, TransportJsonError> {
        self.offset += 1;
        self.skip_whitespace();
        let mut values = Map::new();
        let mut keys = HashSet::new();
        if self.consume_if(b'}') {
            return Ok(Value::Object(values));
        }
        loop {
            if self.peek() != Some(b'"') {
                return self.syntax();
            }
            let key = self.parse_string()?;
            if !keys.insert(key.clone()) {
                return self.syntax();
            }
            self.skip_whitespace();
            self.expect(b':')?;
            self.skip_whitespace();
            values.insert(key, self.parse_value()?);
            self.skip_whitespace();
            if self.consume_if(b'}') {
                return Ok(Value::Object(values));
            }
            self.expect(b',')?;
            self.skip_whitespace();
        }
    }

    fn parse_string(&mut self) -> Result<String, TransportJsonError> {
        self.expect(b'"')?;
        let mut value = String::new();
        let mut segment_start = self.offset;
        while let Some(byte) = self.peek() {
            match byte {
                b'"' => {
                    value.push_str(&self.source[segment_start..self.offset]);
                    self.offset += 1;
                    return Ok(value);
                }
                b'\\' => {
                    value.push_str(&self.source[segment_start..self.offset]);
                    self.offset += 1;
                    self.parse_escape(&mut value)?;
                    segment_start = self.offset;
                }
                0x00..=0x1f => return self.syntax(),
                0x20..=0x7f => self.offset += 1,
                _ => {
                    let character = self.source[self.offset..]
                        .chars()
                        .next()
                        .ok_or_else(|| self.error(TransportJsonErrorKind::Utf8))?;
                    self.offset += character.len_utf8();
                }
            }
        }
        self.syntax()
    }

    fn parse_escape(&mut self, value: &mut String) -> Result<(), TransportJsonError> {
        let escaped = self
            .peek()
            .ok_or_else(|| self.error(TransportJsonErrorKind::Syntax))?;
        self.offset += 1;
        match escaped {
            b'"' => value.push('"'),
            b'\\' => value.push('\\'),
            b'/' => value.push('/'),
            b'b' => value.push('\u{0008}'),
            b'f' => value.push('\u{000c}'),
            b'n' => value.push('\n'),
            b'r' => value.push('\r'),
            b't' => value.push('\t'),
            b'u' => {
                let high = self.parse_hex_code_unit()?;
                let scalar = if (0xd800..=0xdbff).contains(&high) {
                    if self.source.as_bytes().get(self.offset..self.offset + 2) != Some(b"\\u") {
                        return self.syntax();
                    }
                    self.offset += 2;
                    let low = self.parse_hex_code_unit()?;
                    if !(0xdc00..=0xdfff).contains(&low) {
                        return self.syntax();
                    }
                    0x1_0000 + (((u32::from(high) - 0xd800) << 10) | (u32::from(low) - 0xdc00))
                } else if (0xdc00..=0xdfff).contains(&high) {
                    return self.syntax();
                } else {
                    u32::from(high)
                };
                value.push(
                    char::from_u32(scalar)
                        .ok_or_else(|| self.error(TransportJsonErrorKind::Syntax))?,
                );
            }
            _ => return self.syntax(),
        }
        Ok(())
    }

    fn parse_hex_code_unit(&mut self) -> Result<u16, TransportJsonError> {
        let bytes = self.source.as_bytes();
        if self
            .offset
            .checked_add(4)
            .is_none_or(|end| end > bytes.len())
        {
            return self.syntax();
        }
        let mut value = 0u16;
        for _ in 0..4 {
            value = value
                .checked_mul(16)
                .and_then(|current| current.checked_add(u16::from(hex_value(bytes[self.offset])?)))
                .ok_or_else(|| self.error(TransportJsonErrorKind::Syntax))?;
            self.offset += 1;
        }
        Ok(value)
    }

    fn parse_number(&mut self) -> Result<Number, TransportJsonError> {
        let start = self.offset;
        self.consume_if(b'-');
        match self.peek() {
            Some(b'0') => {
                self.offset += 1;
                if matches!(self.peek(), Some(b'0'..=b'9')) {
                    return self.syntax();
                }
            }
            Some(b'1'..=b'9') => self.consume_digits(),
            _ => return self.syntax(),
        }
        if self.consume_if(b'.') {
            if !matches!(self.peek(), Some(b'0'..=b'9')) {
                return self.syntax();
            }
            self.consume_digits();
        }
        if matches!(self.peek(), Some(b'e' | b'E')) {
            self.offset += 1;
            if matches!(self.peek(), Some(b'+' | b'-')) {
                self.offset += 1;
            }
            if !matches!(self.peek(), Some(b'0'..=b'9')) {
                return self.syntax();
            }
            self.consume_digits();
        }
        Ok(Number::from_string_unchecked(
            self.source[start..self.offset].to_owned(),
        ))
    }

    fn consume_digits(&mut self) {
        while matches!(self.peek(), Some(b'0'..=b'9')) {
            self.offset += 1;
        }
    }

    fn consume_literal(&mut self, literal: &[u8]) -> Result<(), TransportJsonError> {
        if self
            .source
            .as_bytes()
            .get(self.offset..self.offset + literal.len())
            != Some(literal)
        {
            return self.syntax();
        }
        self.offset += literal.len();
        Ok(())
    }

    fn expect(&mut self, expected: u8) -> Result<(), TransportJsonError> {
        if !self.consume_if(expected) {
            return self.syntax();
        }
        Ok(())
    }

    fn consume_if(&mut self, expected: u8) -> bool {
        if self.peek() != Some(expected) {
            return false;
        }
        self.offset += 1;
        true
    }

    fn skip_whitespace(&mut self) {
        while matches!(self.peek(), Some(b' ' | b'\t' | b'\r' | b'\n')) {
            self.offset += 1;
        }
    }

    fn peek(&self) -> Option<u8> {
        self.source.as_bytes().get(self.offset).copied()
    }

    fn error(&self, kind: TransportJsonErrorKind) -> TransportJsonError {
        TransportJsonError {
            kind,
            offset: self.offset,
        }
    }

    fn syntax<T>(&self) -> Result<T, TransportJsonError> {
        Err(self.error(TransportJsonErrorKind::Syntax))
    }

    fn limit<T>(&self) -> Result<T, TransportJsonError> {
        Err(self.error(TransportJsonErrorKind::ImplementationLimit))
    }
}

fn hex_value(byte: u8) -> Option<u8> {
    match byte {
        b'0'..=b'9' => Some(byte - b'0'),
        b'a'..=b'f' => Some(byte - b'a' + 10),
        b'A'..=b'F' => Some(byte - b'A' + 10),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::generated_transport::{
        decode_transport_definition_raw, decode_transport_message_raw,
    };

    #[test]
    fn preserves_arbitrary_precision_number_lexemes() {
        let value = parse_transport_json(br#"[1.0,1e0,123456789012345678901234567890,1e10000]"#)
            .expect("valid JSON");
        let values = value.as_array().expect("array");
        assert_eq!(values[0].as_number().expect("number").as_str(), "1.0");
        assert_eq!(values[1].as_number().expect("number").as_str(), "1e0");
        assert_eq!(
            values[2].as_number().expect("number").as_str(),
            "123456789012345678901234567890"
        );
        assert_eq!(values[3].as_number().expect("number").as_str(), "1e10000");
    }

    #[test]
    fn canonicalizes_numbers_and_utf8_sorted_objects() {
        let value = parse_transport_json(
            br#"{"z":1.0,"a":[1e0,-0,1.2300,123000,0.0012],"nested":{"b":2,"a":1}}"#,
        )
        .expect("valid JSON");
        assert_eq!(
            canonical_transport_json(&value).expect("bounded canonical JSON"),
            br#"{"a":[1,0,123e-2,123e3,12e-4],"nested":{"a":1,"b":2},"z":1}"#
        );
    }

    #[test]
    fn rejects_duplicate_decoded_keys() {
        for raw in [
            br#"{"key":1,"key":2}"#.as_slice(),
            br#"{"key":1,"\u006bey":2}"#,
        ] {
            assert_eq!(
                parse_transport_json(raw)
                    .expect_err("duplicate key must fail")
                    .kind(),
                TransportJsonErrorKind::Syntax
            );
        }
    }

    #[test]
    fn accepts_scalar_unicode_and_rejects_unpaired_surrogates() {
        assert_eq!(
            parse_transport_json(br#""\ud83d\ude00""#).expect("paired surrogate"),
            Value::String("😀".to_owned())
        );
        for raw in [
            br#""\ud800""#.as_slice(),
            br#""\udc00""#,
            br#""\udc00\ud800""#,
        ] {
            assert_eq!(
                parse_transport_json(raw)
                    .expect_err("unpaired surrogate must fail")
                    .kind(),
                TransportJsonErrorKind::Syntax
            );
        }
    }

    #[test]
    fn separates_frame_utf8_syntax_and_implementation_limits() {
        assert_eq!(
            parse_transport_json(&vec![b' '; MAX_TRANSPORT_JSON_BYTES + 1])
                .expect_err("oversized JSON must fail")
                .kind(),
            TransportJsonErrorKind::Frame
        );
        assert_eq!(
            parse_transport_json(&[0xff])
                .expect_err("invalid UTF-8 must fail")
                .kind(),
            TransportJsonErrorKind::Utf8
        );
        assert_eq!(
            parse_transport_json(b"01")
                .expect_err("leading zero must fail")
                .kind(),
            TransportJsonErrorKind::Syntax
        );
        let nested = format!("{}0{}", "[".repeat(129), "]".repeat(129));
        assert_eq!(
            parse_transport_json(nested.as_bytes())
                .expect_err("depth overflow must fail")
                .kind(),
            TransportJsonErrorKind::ImplementationLimit
        );
    }

    #[test]
    fn generated_decoder_preserves_true_schema_numbers_and_mathematical_integers() {
        for raw in [
            br#"{"jsonrpc":"2.0","id":"id","result":1e10000}"#.as_slice(),
            br#"{"jsonrpc":"2.0","id":null,"error":{"code":123456789012345678901234567890,"message":"error","data":1e-10000}}"#,
        ] {
            decode_transport_message_raw(raw).expect("unbounded generic Transport number");
        }
        for raw in [b"1.0".as_slice(), b"1e0", b"9007199254740991e0"] {
            decode_transport_definition_raw("safePositiveInteger", raw)
                .expect("mathematical safe integer");
        }
        for raw in [
            b"1.5".as_slice(),
            b"9007199254740992",
            b"9007199254740992e0",
        ] {
            decode_transport_definition_raw("safePositiveInteger", raw)
                .expect_err("fractional or out-of-range integer must fail");
        }
    }

    #[test]
    fn generated_decoder_rejects_pre_dom_ambiguity() {
        for raw in [
            br#"{"jsonrpc":"2.0","id":"id","result":{"key":1,"key":2}}"#.as_slice(),
            br#"{"jsonrpc":"2.0","id":"id","result":"\ud800"}"#,
            br#"{"jsonrpc":"2.0","id":"id","result":{"\ud800":1}}"#,
        ] {
            decode_transport_message_raw(raw)
                .expect_err("duplicate keys and non-scalar Unicode must fail");
        }
        decode_transport_definition_raw("unknownDefinition", b"null")
            .expect_err("unknown definition must fail");
    }
}
