//! Read-only content-reference, preview, and pagination contracts.
//!
//! The types in this module describe content owned by another component. They
//! do not read or write files, Blob stores, network resources, AAP messages,
//! or Qt state. References are content-addressed metadata; an optional page
//! inline is a bounded, untrusted preview only and never grants authority.

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::fmt;

pub const SCHEMA_VERSION: &str = "content-reference/0.1";
pub const PREVIEW_SCHEMA_VERSION: &str = "content-preview/0.1";
pub const INLINE_LIMITS_SCHEMA_VERSION: &str = "content-inline-limits/0.1";
pub const CURSOR_SCHEMA_VERSION: &str = "content-reference-cursor/0.1";
pub const PAGE_SCHEMA_VERSION: &str = "content-reference-page/0.1";

pub const MAX_CONTENT_BYTES: u64 = 16 * 1024 * 1024;
pub const MAX_REFERENCE_BYTES: usize = 192;
pub const MAX_MEDIA_TYPE_BYTES: usize = 128;
pub const MAX_INLINE_ITEM_BYTES: u64 = 64 * 1024;
pub const MAX_INLINE_TOTAL_BYTES: u64 = 256 * 1024;
pub const MAX_PAGE_BYTES: u64 = 64 * 1024;
pub const MAX_PREVIEW_BYTES: u64 = 64 * 1024;
pub const MAX_PREVIEW_LINES: u64 = 1_000_000;
pub const MAX_IMAGE_EDGE: u64 = 8_192;

const CONTENT_REFERENCE_PREFIXES: &[&str] = &[
    "artifact:sha256:",
    "blob:sha256:",
    "command-output:sha256:",
    "content:sha256:",
    "diagnostic-raw:sha256:",
    "image:sha256:",
    "workspace-edit-content:sha256:",
    "workspace-edit-diff:sha256:",
];
const PREVIEW_IDENTITY_PREFIX: &str = "content-preview:sha256:";
const LIMITS_IDENTITY_PREFIX: &str = "content-inline-limits:sha256:";
const BINDING_IDENTITY_PREFIX: &str = "content-reference-binding:sha256:";
const CURSOR_IDENTITY_PREFIX: &str = "content-reference-cursor:sha256:";
const PAGE_IDENTITY_PREFIX: &str = "content-reference-page:sha256:";

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ContentReferenceError {
    pub code: &'static str,
    pub message: &'static str,
}

impl ContentReferenceError {
    fn new(code: &'static str, message: &'static str) -> Self {
        Self { code, message }
    }
}

impl fmt::Display for ContentReferenceError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.message)
    }
}

fn error(code: &'static str, message: &'static str) -> ContentReferenceError {
    ContentReferenceError::new(code, message)
}

fn hex_digest(prefix: &str, values: &[&[u8]]) -> String {
    let mut digest = Sha256::new();
    digest.update(prefix.as_bytes());
    digest.update([0]);
    for value in values {
        digest.update((value.len() as u64).to_be_bytes());
        digest.update(value);
    }
    format!("{prefix}{:x}", digest.finalize())
}

fn valid_hex(value: &str) -> bool {
    value.len() == 64 && value.bytes().all(|byte| byte.is_ascii_hexdigit())
}

fn valid_lower_hex(value: &str) -> bool {
    valid_hex(value) && value.bytes().all(|byte| !byte.is_ascii_uppercase())
}

fn redacted_placeholder(value: &str) -> bool {
    let value = value.trim_start();
    let Some(remainder) = value.strip_prefix("[redacted]") else {
        return false;
    };
    remainder
        .chars()
        .all(|character| character.is_whitespace() || ",;\"'})]".contains(character))
}

fn contains_unredacted_value(value: &str, needle: &str) -> bool {
    let mut remainder = value;
    while let Some(index) = remainder.find(needle) {
        let candidate = &remainder[index + needle.len()..];
        let field_value = candidate
            .split_once(['\r', '\n'])
            .map_or(candidate, |(line, _)| line);
        if !field_value.trim().is_empty() && !redacted_placeholder(field_value) {
            return true;
        }
        remainder = candidate;
    }
    false
}

fn secret_value_shaped(value: &str) -> bool {
    let lower = value.to_ascii_lowercase();
    let jwt = value.split('.').collect::<Vec<_>>();
    (jwt.len() == 3
        && jwt.iter().all(|segment| {
            segment.len() >= 8
                && segment
                    .bytes()
                    .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_'))
        }))
        || contains_unredacted_value(&lower, "api_key=")
        || contains_unredacted_value(&lower, "api-key=")
        || contains_unredacted_value(&lower, "access_token=")
        || contains_unredacted_value(&lower, "access-token=")
        || contains_unredacted_value(&lower, "refresh_token=")
        || contains_unredacted_value(&lower, "refresh-token=")
        || contains_unredacted_value(&lower, "authorization: bearer")
        || contains_unredacted_value(&lower, "authorization=bearer")
        || value
            .split(|character: char| {
                !character.is_ascii_alphanumeric() && character != '_' && character != '-'
            })
            .any(|token| {
                (token.starts_with("sk-") && token.len() >= 20)
                    || (token.starts_with("ghp_") && token.len() >= 20)
                    || (token.starts_with("github_pat_") && token.len() >= 24)
                    || (token.starts_with("xoxb-") && token.len() >= 20)
            })
}

fn valid_sha_identity(value: &str, prefix: &str) -> bool {
    value.len() == prefix.len() + 64
        && value.starts_with(prefix)
        && valid_lower_hex(&value[prefix.len()..])
}

fn valid_media_type(value: &str) -> bool {
    value.len() <= MAX_MEDIA_TYPE_BYTES
        && matches!(
            value,
            "application/json"
                | "application/octet-stream"
                | "image/jpeg"
                | "image/png"
                | "image/webp"
                | "text/plain; charset=utf-8"
                | "text/x-diff; charset=utf-8"
        )
}

fn is_text_media_type(value: &str) -> bool {
    matches!(
        value,
        "application/json" | "text/plain; charset=utf-8" | "text/x-diff; charset=utf-8"
    )
}

fn is_image_media_type(value: &str) -> bool {
    matches!(value, "image/jpeg" | "image/png" | "image/webp")
}

fn validate_reference(reference: &str, sha256: &str) -> Result<(), ContentReferenceError> {
    if reference.len() > MAX_REFERENCE_BYTES
        || !CONTENT_REFERENCE_PREFIXES
            .iter()
            .any(|prefix| reference.starts_with(prefix))
        || !valid_lower_hex(sha256)
    {
        return Err(error(
            "content-reference-identity-invalid",
            "content reference identity is invalid",
        ));
    }
    let prefix = CONTENT_REFERENCE_PREFIXES
        .iter()
        .find(|prefix| reference.starts_with(**prefix))
        .expect("validated content reference prefix");
    if reference != format!("{prefix}{sha256}") {
        return Err(error(
            "content-reference-hash-mismatch",
            "content reference does not bind its SHA-256",
        ));
    }
    Ok(())
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(try_from = "InlineSizeLimitsWire")]
#[serde(deny_unknown_fields)]
pub struct InlineSizeLimits {
    pub schema_version: String,
    pub max_item_bytes: u64,
    pub max_total_bytes: u64,
    pub identity: String,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct InlineSizeLimitsWire {
    schema_version: String,
    max_item_bytes: u64,
    max_total_bytes: u64,
    identity: String,
}

impl TryFrom<InlineSizeLimitsWire> for InlineSizeLimits {
    type Error = String;

    fn try_from(wire: InlineSizeLimitsWire) -> Result<Self, Self::Error> {
        let limits = Self {
            schema_version: wire.schema_version,
            max_item_bytes: wire.max_item_bytes,
            max_total_bytes: wire.max_total_bytes,
            identity: wire.identity,
        };
        limits
            .validate()
            .map_err(|error| error.message.to_owned())?;
        Ok(limits)
    }
}

impl InlineSizeLimits {
    pub fn new(max_item_bytes: u64, max_total_bytes: u64) -> Result<Self, ContentReferenceError> {
        let limits = Self {
            schema_version: INLINE_LIMITS_SCHEMA_VERSION.into(),
            max_item_bytes,
            max_total_bytes,
            identity: String::new(),
        };
        let mut limits = limits;
        limits.identity = limits.derived_identity();
        limits.validate()?;
        Ok(limits)
    }

    /// Negotiation is an intersection of caller and peer limits. It never
    /// widens either side or enables a content producer.
    pub fn negotiate(
        local_item_bytes: u64,
        local_total_bytes: u64,
        peer_item_bytes: u64,
        peer_total_bytes: u64,
    ) -> Result<Self, ContentReferenceError> {
        let max_total_bytes = local_total_bytes.min(peer_total_bytes);
        Self::new(
            local_item_bytes.min(peer_item_bytes).min(max_total_bytes),
            max_total_bytes,
        )
    }

    fn derived_identity(&self) -> String {
        hex_digest(
            LIMITS_IDENTITY_PREFIX,
            &[
                INLINE_LIMITS_SCHEMA_VERSION.as_bytes(),
                &self.max_item_bytes.to_be_bytes(),
                &self.max_total_bytes.to_be_bytes(),
            ],
        )
    }

    pub fn validate(&self) -> Result<(), ContentReferenceError> {
        if self.schema_version != INLINE_LIMITS_SCHEMA_VERSION
            || self.max_item_bytes == 0
            || self.max_item_bytes > MAX_INLINE_ITEM_BYTES
            || self.max_total_bytes == 0
            || self.max_total_bytes > MAX_INLINE_TOTAL_BYTES
            || self.max_item_bytes > self.max_total_bytes
            || !valid_sha_identity(&self.identity, LIMITS_IDENTITY_PREFIX)
            || self.identity != self.derived_identity()
        {
            return Err(error(
                "content-inline-limits-invalid",
                "content inline-size limits are invalid",
            ));
        }
        Ok(())
    }

    pub fn allows(&self, item_bytes: u64, total_bytes: u64) -> bool {
        self.validate().is_ok()
            && item_bytes <= self.max_item_bytes
            && total_bytes <= self.max_total_bytes
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(try_from = "ContentPreviewMetadataWire")]
#[serde(deny_unknown_fields)]
pub struct ContentPreviewMetadata {
    pub schema_version: String,
    pub identity: String,
    pub reference: String,
    pub sha256: String,
    pub media_type: String,
    pub content_bytes: u64,
    pub preview_bytes: u64,
    pub truncated: bool,
    pub line_count: Option<u64>,
    pub width: Option<u64>,
    pub height: Option<u64>,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct ContentPreviewMetadataWire {
    schema_version: String,
    identity: String,
    reference: String,
    sha256: String,
    media_type: String,
    content_bytes: u64,
    preview_bytes: u64,
    truncated: bool,
    line_count: Option<u64>,
    width: Option<u64>,
    height: Option<u64>,
}

impl TryFrom<ContentPreviewMetadataWire> for ContentPreviewMetadata {
    type Error = String;

    fn try_from(wire: ContentPreviewMetadataWire) -> Result<Self, Self::Error> {
        let preview = Self {
            schema_version: wire.schema_version,
            identity: wire.identity,
            reference: wire.reference,
            sha256: wire.sha256,
            media_type: wire.media_type,
            content_bytes: wire.content_bytes,
            preview_bytes: wire.preview_bytes,
            truncated: wire.truncated,
            line_count: wire.line_count,
            width: wire.width,
            height: wire.height,
        };
        preview
            .validate()
            .map_err(|error| error.message.to_owned())?;
        Ok(preview)
    }
}

impl ContentPreviewMetadata {
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        reference: impl Into<String>,
        sha256: impl Into<String>,
        media_type: impl Into<String>,
        content_bytes: u64,
        preview_bytes: u64,
        truncated: bool,
        line_count: Option<u64>,
        width: Option<u64>,
        height: Option<u64>,
    ) -> Result<Self, ContentReferenceError> {
        let mut preview = Self {
            schema_version: PREVIEW_SCHEMA_VERSION.into(),
            identity: String::new(),
            reference: reference.into(),
            sha256: sha256.into(),
            media_type: media_type.into(),
            content_bytes,
            preview_bytes,
            truncated,
            line_count,
            width,
            height,
        };
        preview.identity = preview.derived_identity();
        preview.validate()?;
        Ok(preview)
    }

    fn derived_identity(&self) -> String {
        let line_count = self.line_count.map_or(u64::MAX, |value| value);
        let width = self.width.map_or(u64::MAX, |value| value);
        let height = self.height.map_or(u64::MAX, |value| value);
        hex_digest(
            PREVIEW_IDENTITY_PREFIX,
            &[
                self.reference.as_bytes(),
                self.sha256.as_bytes(),
                self.media_type.as_bytes(),
                &self.content_bytes.to_be_bytes(),
                &self.preview_bytes.to_be_bytes(),
                &[u8::from(self.truncated)],
                &line_count.to_be_bytes(),
                &width.to_be_bytes(),
                &height.to_be_bytes(),
            ],
        )
    }

    pub fn validate(&self) -> Result<(), ContentReferenceError> {
        if self.schema_version != PREVIEW_SCHEMA_VERSION
            || !valid_sha_identity(&self.identity, PREVIEW_IDENTITY_PREFIX)
            || !valid_media_type(&self.media_type)
            || self.content_bytes > MAX_CONTENT_BYTES
            || self.preview_bytes > self.content_bytes
            || self.preview_bytes > MAX_PREVIEW_BYTES
            || self.truncated != (self.preview_bytes < self.content_bytes)
            || self.identity != self.derived_identity()
        {
            return Err(error(
                "content-preview-invalid",
                "content preview metadata is invalid",
            ));
        }
        validate_reference(&self.reference, &self.sha256)?;
        if self.line_count.is_some_and(|value| {
            value > MAX_PREVIEW_LINES || value > self.content_bytes.saturating_add(1)
        }) {
            return Err(error(
                "content-preview-lines-invalid",
                "content preview line count is outside its bound",
            ));
        }
        if is_image_media_type(&self.media_type) {
            if self.width.is_none() || self.height.is_none() {
                return Err(error(
                    "content-preview-dimensions-invalid",
                    "image preview metadata requires both dimensions",
                ));
            }
        } else if self.width.is_some() || self.height.is_some() {
            return Err(error(
                "content-preview-dimensions-invalid",
                "non-image preview metadata cannot carry dimensions",
            ));
        }
        for dimension in [self.width, self.height].into_iter().flatten() {
            if dimension == 0 || dimension > MAX_IMAGE_EDGE {
                return Err(error(
                    "content-preview-dimensions-invalid",
                    "content preview dimension is outside its bound",
                ));
            }
        }
        if !is_text_media_type(&self.media_type) && self.line_count.is_some() {
            return Err(error(
                "content-preview-lines-invalid",
                "non-text preview metadata cannot carry line counts",
            ));
        }
        Ok(())
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(try_from = "ContentReferenceWire")]
#[serde(deny_unknown_fields)]
pub struct ContentReference {
    pub schema_version: String,
    pub reference: String,
    pub sha256: String,
    pub bytes: u64,
    pub media_type: String,
    pub preview: Option<ContentPreviewMetadata>,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct ContentReferenceWire {
    schema_version: String,
    reference: String,
    sha256: String,
    bytes: u64,
    media_type: String,
    preview: Option<ContentPreviewMetadata>,
}

impl TryFrom<ContentReferenceWire> for ContentReference {
    type Error = String;

    fn try_from(wire: ContentReferenceWire) -> Result<Self, Self::Error> {
        let content = Self {
            schema_version: wire.schema_version,
            reference: wire.reference,
            sha256: wire.sha256,
            bytes: wire.bytes,
            media_type: wire.media_type,
            preview: wire.preview,
        };
        content
            .validate()
            .map_err(|error| error.message.to_owned())?;
        Ok(content)
    }
}

impl ContentReference {
    pub fn new(
        reference: impl Into<String>,
        sha256: impl Into<String>,
        bytes: u64,
        media_type: impl Into<String>,
        preview: Option<ContentPreviewMetadata>,
    ) -> Result<Self, ContentReferenceError> {
        let content = Self {
            schema_version: SCHEMA_VERSION.into(),
            reference: reference.into(),
            sha256: sha256.into(),
            bytes,
            media_type: media_type.into(),
            preview,
        };
        content.validate()?;
        Ok(content)
    }

    pub fn with_preview(
        mut self,
        preview_bytes: u64,
        truncated: bool,
        line_count: Option<u64>,
        width: Option<u64>,
        height: Option<u64>,
    ) -> Result<Self, ContentReferenceError> {
        self.preview = Some(ContentPreviewMetadata::new(
            self.reference.clone(),
            self.sha256.clone(),
            self.media_type.clone(),
            self.bytes,
            preview_bytes,
            truncated,
            line_count,
            width,
            height,
        )?);
        self.validate()?;
        Ok(self)
    }

    pub fn validate(&self) -> Result<(), ContentReferenceError> {
        if self.schema_version != SCHEMA_VERSION
            || self.bytes > MAX_CONTENT_BYTES
            || !valid_media_type(&self.media_type)
        {
            return Err(error(
                "content-reference-invalid",
                "content reference schema, size, or MIME type is invalid",
            ));
        }
        validate_reference(&self.reference, &self.sha256)?;
        if let Some(preview) = &self.preview {
            preview.validate()?;
            if preview.reference != self.reference
                || preview.sha256 != self.sha256
                || preview.media_type != self.media_type
                || preview.content_bytes != self.bytes
            {
                return Err(error(
                    "content-preview-binding-invalid",
                    "content preview is not bound to the exact content reference",
                ));
            }
        }
        Ok(())
    }

    pub fn page(
        &self,
        limits: InlineSizeLimits,
        offset: u64,
        page_size: u64,
        page_bytes: u64,
        inline: Option<String>,
    ) -> Result<ContentPage, ContentReferenceError> {
        self.page_with_binding(limits, offset, page_size, page_bytes, inline, None)
    }

    pub fn page_with_binding(
        &self,
        limits: InlineSizeLimits,
        offset: u64,
        page_size: u64,
        page_bytes: u64,
        inline: Option<String>,
        binding_identity: Option<String>,
    ) -> Result<ContentPage, ContentReferenceError> {
        self.page_with_binding_internal(
            limits,
            offset,
            page_size,
            page_bytes,
            inline,
            binding_identity,
            true,
        )
    }

    pub fn page_text_with_binding(
        &self,
        limits: InlineSizeLimits,
        offset: u64,
        page_size: u64,
        full_text: &str,
        binding_identity: Option<String>,
    ) -> Result<ContentPage, ContentReferenceError> {
        self.validate()?;
        limits.validate()?;
        let full_text_hash = format!("{:x}", Sha256::digest(full_text.as_bytes()));
        if !is_text_media_type(&self.media_type)
            || full_text.len() as u64 != self.bytes
            || full_text_hash != self.sha256
            || secret_value_shaped(full_text)
        {
            return Err(error(
                "content-page-source-invalid",
                "content page source is invalid or contains an unsafe secret shape",
            ));
        }
        let start = usize::try_from(offset).map_err(|_| {
            error(
                "content-page-window-invalid",
                "content page offset is outside its byte bounds",
            )
        })?;
        let page_size_usize = usize::try_from(page_size).map_err(|_| {
            error(
                "content-page-window-invalid",
                "content page size is outside its byte bounds",
            )
        })?;
        if start > full_text.len() || !full_text.is_char_boundary(start) {
            return Err(error(
                "content-page-window-invalid",
                "content page offset is not a UTF-8 boundary",
            ));
        }
        let mut end = start.saturating_add(page_size_usize).min(full_text.len());
        while end > start && !full_text.is_char_boundary(end) {
            end -= 1;
        }
        if end == start && start < full_text.len() {
            return Err(error(
                "content-page-scalar-limit-invalid",
                "content page size cannot contain the next UTF-8 scalar",
            ));
        }
        self.page_with_binding_internal(
            limits,
            offset,
            page_size,
            (end - start) as u64,
            Some(full_text[start..end].to_owned()),
            binding_identity,
            false,
        )
    }

    #[allow(clippy::too_many_arguments)]
    fn page_with_binding_internal(
        &self,
        limits: InlineSizeLimits,
        offset: u64,
        page_size: u64,
        page_bytes: u64,
        inline: Option<String>,
        binding_identity: Option<String>,
        reject_inline_secret_shape: bool,
    ) -> Result<ContentPage, ContentReferenceError> {
        self.validate()?;
        limits.validate()?;
        if binding_identity
            .as_deref()
            .is_some_and(|identity| !valid_sha_identity(identity, BINDING_IDENTITY_PREFIX))
        {
            return Err(error(
                "content-page-binding-invalid",
                "content page binding identity is invalid",
            ));
        }
        if page_size == 0
            || page_size > MAX_PAGE_BYTES
            || page_size > limits.max_item_bytes
            || offset > self.bytes
            || page_bytes > page_size
            || page_bytes > self.bytes.saturating_sub(offset)
            || (self.bytes > 0 && page_bytes == 0)
        {
            return Err(error(
                "content-page-window-invalid",
                "content page window is outside its byte bounds",
            ));
        }
        let inline_bytes = inline.as_ref().map_or(0, |value| value.len() as u64);
        if inline_bytes > page_bytes
            || !limits.allows(inline_bytes, inline_bytes)
            || (!is_text_media_type(&self.media_type) && inline.is_some())
            || (reject_inline_secret_shape
                && inline
                    .as_ref()
                    .is_some_and(|value| secret_value_shaped(value)))
        {
            return Err(error(
                "content-page-inline-invalid",
                "content page inline preview is invalid or exceeds its negotiated limit",
            ));
        }
        let next_offset = offset.saturating_add(page_bytes);
        let next_cursor = (next_offset < self.bytes).then(|| ContentPageCursor {
            schema_version: CURSOR_SCHEMA_VERSION.into(),
            reference: self.reference.clone(),
            sha256: self.sha256.clone(),
            bytes: self.bytes,
            media_type: self.media_type.clone(),
            offset: next_offset,
            page_size,
            limits: limits.clone(),
            binding_identity: binding_identity.clone(),
            identity: String::new(),
        });
        let next_cursor = next_cursor.map(|mut cursor| {
            cursor.identity = cursor.derived_identity();
            cursor
        });
        let mut page = ContentPage {
            schema_version: PAGE_SCHEMA_VERSION.into(),
            reference: self.reference.clone(),
            sha256: self.sha256.clone(),
            bytes: self.bytes,
            media_type: self.media_type.clone(),
            offset,
            page_size,
            page_bytes,
            inline,
            inline_truncated: inline_bytes < page_bytes,
            limits,
            binding_identity,
            next_cursor,
            identity: String::new(),
        };
        page.identity = page.derived_identity();
        page.validate()?;
        Ok(page)
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(try_from = "ContentPageCursorWire")]
#[serde(deny_unknown_fields)]
pub struct ContentPageCursor {
    pub schema_version: String,
    pub reference: String,
    pub sha256: String,
    pub bytes: u64,
    pub media_type: String,
    pub offset: u64,
    pub page_size: u64,
    pub limits: InlineSizeLimits,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub binding_identity: Option<String>,
    pub identity: String,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct ContentPageCursorWire {
    schema_version: String,
    reference: String,
    sha256: String,
    bytes: u64,
    media_type: String,
    offset: u64,
    page_size: u64,
    limits: InlineSizeLimits,
    #[serde(default)]
    binding_identity: Option<String>,
    identity: String,
}

impl TryFrom<ContentPageCursorWire> for ContentPageCursor {
    type Error = String;

    fn try_from(wire: ContentPageCursorWire) -> Result<Self, Self::Error> {
        let cursor = Self {
            schema_version: wire.schema_version,
            reference: wire.reference,
            sha256: wire.sha256,
            bytes: wire.bytes,
            media_type: wire.media_type,
            offset: wire.offset,
            page_size: wire.page_size,
            limits: wire.limits,
            binding_identity: wire.binding_identity,
            identity: wire.identity,
        };
        cursor
            .validate()
            .map_err(|error| error.message.to_owned())?;
        Ok(cursor)
    }
}

impl ContentPageCursor {
    fn derived_identity(&self) -> String {
        hex_digest(
            CURSOR_IDENTITY_PREFIX,
            &[
                self.reference.as_bytes(),
                self.sha256.as_bytes(),
                &self.bytes.to_be_bytes(),
                self.media_type.as_bytes(),
                &self.offset.to_be_bytes(),
                &self.page_size.to_be_bytes(),
                self.limits.identity.as_bytes(),
                self.binding_identity
                    .as_deref()
                    .unwrap_or("none")
                    .as_bytes(),
            ],
        )
    }

    pub fn validate(&self) -> Result<(), ContentReferenceError> {
        if self.schema_version != CURSOR_SCHEMA_VERSION
            || !valid_sha_identity(&self.identity, CURSOR_IDENTITY_PREFIX)
            || self.bytes > MAX_CONTENT_BYTES
            || self.offset > self.bytes
            || self.page_size == 0
            || self.page_size > MAX_PAGE_BYTES
            || self.page_size > self.limits.max_item_bytes
            || self
                .binding_identity
                .as_deref()
                .is_some_and(|identity| !valid_sha_identity(identity, BINDING_IDENTITY_PREFIX))
            || self.identity != self.derived_identity()
        {
            return Err(error(
                "content-cursor-invalid",
                "content page cursor is invalid",
            ));
        }
        validate_reference(&self.reference, &self.sha256)?;
        self.limits.validate()?;
        if !valid_media_type(&self.media_type) {
            return Err(error(
                "content-cursor-media-type-invalid",
                "content page cursor MIME type is invalid",
            ));
        }
        Ok(())
    }

    pub fn matches(&self, reference: &ContentReference) -> bool {
        self.validate().is_ok()
            && reference.validate().is_ok()
            && self.reference == reference.reference
            && self.sha256 == reference.sha256
            && self.bytes == reference.bytes
            && self.media_type == reference.media_type
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(try_from = "ContentPageWire")]
#[serde(deny_unknown_fields)]
pub struct ContentPage {
    pub schema_version: String,
    pub reference: String,
    pub sha256: String,
    pub bytes: u64,
    pub media_type: String,
    pub offset: u64,
    pub page_size: u64,
    pub page_bytes: u64,
    pub inline: Option<String>,
    pub inline_truncated: bool,
    pub limits: InlineSizeLimits,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub binding_identity: Option<String>,
    pub next_cursor: Option<ContentPageCursor>,
    pub identity: String,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct ContentPageWire {
    schema_version: String,
    reference: String,
    sha256: String,
    bytes: u64,
    media_type: String,
    offset: u64,
    page_size: u64,
    page_bytes: u64,
    inline: Option<String>,
    inline_truncated: bool,
    limits: InlineSizeLimits,
    #[serde(default)]
    binding_identity: Option<String>,
    next_cursor: Option<ContentPageCursor>,
    identity: String,
}

impl TryFrom<ContentPageWire> for ContentPage {
    type Error = String;

    fn try_from(wire: ContentPageWire) -> Result<Self, Self::Error> {
        let page = Self {
            schema_version: wire.schema_version,
            reference: wire.reference,
            sha256: wire.sha256,
            bytes: wire.bytes,
            media_type: wire.media_type,
            offset: wire.offset,
            page_size: wire.page_size,
            page_bytes: wire.page_bytes,
            inline: wire.inline,
            inline_truncated: wire.inline_truncated,
            limits: wire.limits,
            binding_identity: wire.binding_identity,
            next_cursor: wire.next_cursor,
            identity: wire.identity,
        };
        page.validate().map_err(|error| error.message.to_owned())?;
        Ok(page)
    }
}

impl ContentPage {
    fn derived_identity(&self) -> String {
        let inline_hash = self.inline.as_ref().map_or_else(
            || Sha256::digest([]).to_vec(),
            |value| Sha256::digest(value.as_bytes()).to_vec(),
        );
        let next_identity = self
            .next_cursor
            .as_ref()
            .map_or_else(|| "none".to_owned(), |cursor| cursor.identity.clone());
        hex_digest(
            PAGE_IDENTITY_PREFIX,
            &[
                self.reference.as_bytes(),
                self.sha256.as_bytes(),
                &self.bytes.to_be_bytes(),
                self.media_type.as_bytes(),
                &self.offset.to_be_bytes(),
                &self.page_size.to_be_bytes(),
                &self.page_bytes.to_be_bytes(),
                &inline_hash,
                &[u8::from(self.inline_truncated)],
                self.limits.identity.as_bytes(),
                self.binding_identity
                    .as_deref()
                    .unwrap_or("none")
                    .as_bytes(),
                next_identity.as_bytes(),
            ],
        )
    }

    pub fn validate(&self) -> Result<(), ContentReferenceError> {
        if self.schema_version != PAGE_SCHEMA_VERSION
            || !valid_sha_identity(&self.identity, PAGE_IDENTITY_PREFIX)
            || self.bytes > MAX_CONTENT_BYTES
            || self.page_size == 0
            || self.page_size > MAX_PAGE_BYTES
            || self.page_size > self.limits.max_item_bytes
            || self.offset > self.bytes
            || self.page_bytes > self.page_size
            || self.page_bytes > self.bytes.saturating_sub(self.offset)
            || (self.bytes > 0 && self.page_bytes == 0)
            || self
                .binding_identity
                .as_deref()
                .is_some_and(|identity| !valid_sha_identity(identity, BINDING_IDENTITY_PREFIX))
            || self.identity != self.derived_identity()
        {
            return Err(error(
                "content-page-invalid",
                "content page metadata or identity is invalid",
            ));
        }
        validate_reference(&self.reference, &self.sha256)?;
        self.limits.validate()?;
        if !valid_media_type(&self.media_type) {
            return Err(error(
                "content-page-media-type-invalid",
                "content page MIME type is invalid",
            ));
        }
        let inline_bytes = self.inline.as_ref().map_or(0, |value| value.len() as u64);
        if inline_bytes > self.page_bytes
            || !self.limits.allows(inline_bytes, inline_bytes)
            || self.inline_truncated != (inline_bytes < self.page_bytes)
            || (!is_text_media_type(&self.media_type) && self.inline.is_some())
        {
            return Err(error(
                "content-page-inline-invalid",
                "content page inline preview is invalid",
            ));
        }
        if let Some(cursor) = &self.next_cursor {
            cursor.validate()?;
            if !cursor.matches(&ContentReference {
                schema_version: SCHEMA_VERSION.into(),
                reference: self.reference.clone(),
                sha256: self.sha256.clone(),
                bytes: self.bytes,
                media_type: self.media_type.clone(),
                preview: None,
            }) || cursor.offset != self.offset.saturating_add(self.page_bytes)
                || cursor.page_size != self.page_size
                || cursor.limits != self.limits
                || cursor.binding_identity != self.binding_identity
            {
                return Err(error(
                    "content-page-cursor-invalid",
                    "content page cursor does not bind the next byte window",
                ));
            }
        } else if self.offset.saturating_add(self.page_bytes) != self.bytes {
            return Err(error(
                "content-page-cursor-invalid",
                "non-terminal content page must carry a next cursor",
            ));
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn sha(byte: char) -> String {
        byte.to_string().repeat(64)
    }

    fn reference(byte: char) -> String {
        format!("content:sha256:{}", byte.to_string().repeat(64))
    }

    fn limits() -> InlineSizeLimits {
        InlineSizeLimits::negotiate(64 * 1024, 256 * 1024, 32 * 1024, 128 * 1024).unwrap()
    }

    fn content() -> ContentReference {
        ContentReference::new(
            reference('a'),
            sha('a'),
            100_000,
            "text/plain; charset=utf-8",
            None,
        )
        .unwrap()
    }

    #[test]
    fn references_bind_lowercase_hash_domain_size_and_mime_allowlist() {
        let content = content()
            .with_preview(32_000, true, Some(400), None, None)
            .unwrap();
        assert!(content.validate().is_ok());
        assert_eq!(content.preview.as_ref().unwrap().content_bytes, 100_000);
        assert!(ContentReference::new(
            "content:sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaA",
            sha('a'),
            1,
            "text/plain; charset=utf-8",
            None,
        )
        .is_err());
        assert!(ContentReference::new(
            "unknown:sha256:".to_owned() + &sha('a'),
            sha('a'),
            1,
            "text/plain; charset=utf-8",
            None,
        )
        .is_err());
        assert!(
            ContentReference::new(reference('a'), sha('a'), 1, "text/x-unknown", None,).is_err()
        );
        assert!(ContentReference::new(
            reference('a'),
            sha('a'),
            MAX_CONTENT_BYTES + 1,
            "application/json",
            None,
        )
        .is_err());
    }

    #[test]
    fn preview_metadata_rejects_binding_drift_and_bad_dimensions() {
        let content = ContentReference::new(
            "image:sha256:".to_owned() + &sha('b'),
            sha('b'),
            2_000,
            "image/png",
            None,
        )
        .unwrap()
        .with_preview(2_000, false, None, Some(64), Some(48))
        .unwrap();
        assert!(content.validate().is_ok());
        let mut forged = content.clone();
        forged.preview.as_mut().unwrap().width = Some(65);
        assert!(forged.validate().is_err());
        assert!(ContentPreviewMetadata::new(
            content.reference.clone(),
            content.sha256.clone(),
            content.media_type.clone(),
            content.bytes,
            1,
            true,
            None,
            None,
            None,
        )
        .is_err());
    }

    #[test]
    fn negotiated_limits_intersect_and_pages_bind_cursor_and_identity() {
        let content = content();
        let negotiated = limits();
        assert_eq!(negotiated.max_item_bytes, 32 * 1024);
        assert_eq!(negotiated.max_total_bytes, 128 * 1024);
        assert!(negotiated.allows(32 * 1024, 128 * 1024));
        assert!(!negotiated.allows(32 * 1024 + 1, 1));
        let total_below_item =
            InlineSizeLimits::negotiate(64 * 1024, 256 * 1024, 64 * 1024, 4 * 1024).unwrap();
        assert_eq!(total_below_item.max_item_bytes, 4 * 1024);
        assert_eq!(total_below_item.max_total_bytes, 4 * 1024);

        let first = content
            .page(
                negotiated.clone(),
                0,
                32 * 1024,
                32 * 1024,
                Some("x".repeat(32 * 1024)),
            )
            .unwrap();
        assert!(first.next_cursor.is_some());
        assert!(first.validate().is_ok());
        let cursor = first.next_cursor.clone().unwrap();
        assert!(cursor.matches(&content));
        let second = content
            .page(
                negotiated,
                cursor.offset,
                cursor.page_size,
                1_000,
                Some("y".repeat(1_000)),
            )
            .unwrap();
        assert_ne!(first.identity, second.identity);
        assert_eq!(second.offset, 32 * 1024);

        let binding = format!("content-reference-binding:sha256:{}", sha('c'));
        let scoped = content
            .page_with_binding(
                limits(),
                0,
                4,
                4,
                Some("safe".into()),
                Some(binding.clone()),
            )
            .unwrap();
        assert_eq!(scoped.binding_identity.as_deref(), Some(binding.as_str()));
        assert_eq!(
            scoped
                .next_cursor
                .as_ref()
                .and_then(|cursor| cursor.binding_identity.as_deref()),
            Some(binding.as_str())
        );
        let mut drifted = scoped;
        drifted.binding_identity = Some(format!("content-reference-binding:sha256:{}", sha('d')));
        assert!(drifted.validate().is_err());
    }

    #[test]
    fn serde_unknown_fields_and_secret_shaped_inline_fail_closed() {
        let content = content();
        let mut encoded = serde_json::to_value(&content).unwrap();
        encoded["authorization"] = json!("Bearer secret");
        assert!(serde_json::from_value::<ContentReference>(encoded).is_err());

        let mut forged_mime = serde_json::to_value(&content).unwrap();
        forged_mime["media_type"] = json!("text/x-unknown");
        assert!(serde_json::from_value::<ContentReference>(forged_mime).is_err());

        let mut page = content
            .page(limits(), 0, 4, 4, Some("safe".into()))
            .unwrap();
        page.inline = Some("api_key=sk-123456789012345678901234".into());
        assert!(page.validate().is_err());
        let mut encoded = serde_json::to_value(&page).unwrap();
        assert!(serde_json::from_value::<ContentPage>(encoded.clone()).is_err());
        encoded["provider_body"] = json!("must not persist");
        assert!(serde_json::from_value::<ContentPage>(encoded).is_err());

        let safe_redacted = "API_KEY=[REDACTED]\nAuthorization: Bearer [REDACTED]".to_owned();
        let redacted = content
            .page(
                limits(),
                0,
                safe_redacted.len() as u64,
                safe_redacted.len() as u64,
                Some(safe_redacted),
            )
            .unwrap();
        assert!(redacted.validate().is_ok());

        for leaked in [
            "API_KEY=[REDACTED],actual-secret",
            "Authorization: Bearer [REDACTED] actual-secret",
        ] {
            assert!(content
                .page(
                    limits(),
                    0,
                    leaked.len() as u64,
                    leaked.len() as u64,
                    Some(leaked.into()),
                )
                .is_err());
        }

        let safe_full = "API_KEY=[REDACTED]\nAuthorization: Bearer [REDACTED]";
        let safe_hash = format!("{:x}", Sha256::digest(safe_full.as_bytes()));
        let safe_reference = ContentReference::new(
            format!("content:sha256:{safe_hash}"),
            safe_hash,
            safe_full.len() as u64,
            "text/plain; charset=utf-8",
            None,
        )
        .unwrap();
        let split_placeholder = safe_reference
            .page_text_with_binding(limits(), 0, 10, safe_full, None)
            .unwrap();
        assert_eq!(split_placeholder.inline.as_deref(), Some("API_KEY=[R"));
        assert!(split_placeholder.validate().is_ok());

        let unsafe_full = "API_KEY=actual-secret-value";
        let unsafe_hash = format!("{:x}", Sha256::digest(unsafe_full.as_bytes()));
        let unsafe_reference = ContentReference::new(
            format!("content:sha256:{unsafe_hash}"),
            unsafe_hash,
            unsafe_full.len() as u64,
            "text/plain; charset=utf-8",
            None,
        )
        .unwrap();
        assert!(unsafe_reference
            .page_text_with_binding(limits(), 0, 8, unsafe_full, None)
            .is_err());
    }
}
