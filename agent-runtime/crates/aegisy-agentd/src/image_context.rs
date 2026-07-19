use image::{DynamicImage, ImageFormat, ImageReader, Limits};
use sha2::{Digest, Sha256};
use std::io::Cursor;

pub const MAX_IMAGE_BYTES: usize = 8 * 1024 * 1024;
pub const MAX_IMAGE_EDGE: u32 = 8_192;
pub const MAX_IMAGE_PIXELS: u64 = 40_000_000;
const MAX_IMAGE_DECODE_ALLOC_BYTES: u64 = 192 * 1024 * 1024;
const THUMBNAIL_MAX_WIDTH: u32 = 320;
const THUMBNAIL_MAX_HEIGHT: u32 = 180;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ImageMetadata {
    pub media_type: &'static str,
    pub extension: &'static str,
    pub width: u32,
    pub height: u32,
    pub bytes: u64,
    pub sha256: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ImageContextError {
    pub code: &'static str,
}

pub fn validate_image(
    bytes: &[u8],
    expected_media_type: Option<&str>,
) -> Result<ImageMetadata, ImageContextError> {
    let (image, format) = decode_image(bytes)?;
    let (media_type, extension) = format_identity(format)?;
    if expected_media_type.is_some_and(|expected| expected != media_type) {
        return Err(error("image-media-type-mismatch"));
    }
    let width = image.width();
    let height = image.height();
    validate_dimensions(width, height)?;
    Ok(ImageMetadata {
        media_type,
        extension,
        width,
        height,
        bytes: bytes.len() as u64,
        sha256: format!("{:x}", Sha256::digest(bytes)),
    })
}

pub fn thumbnail_png(
    bytes: &[u8],
    expected_media_type: &str,
) -> Result<Vec<u8>, ImageContextError> {
    let (image, format) = decode_image(bytes)?;
    let (media_type, _) = format_identity(format)?;
    if media_type != expected_media_type {
        return Err(error("image-media-type-mismatch"));
    }
    validate_dimensions(image.width(), image.height())?;
    let thumbnail =
        if image.width() <= THUMBNAIL_MAX_WIDTH && image.height() <= THUMBNAIL_MAX_HEIGHT {
            image
        } else {
            image.thumbnail(THUMBNAIL_MAX_WIDTH, THUMBNAIL_MAX_HEIGHT)
        };
    let mut output = Cursor::new(Vec::new());
    thumbnail
        .write_to(&mut output, ImageFormat::Png)
        .map_err(|_| error("image-thumbnail-encode-failed"))?;
    Ok(output.into_inner())
}

fn decode_image(bytes: &[u8]) -> Result<(DynamicImage, ImageFormat), ImageContextError> {
    if bytes.is_empty() || bytes.len() > MAX_IMAGE_BYTES {
        return Err(error("image-size-invalid"));
    }
    let format = ImageReader::new(Cursor::new(bytes))
        .with_guessed_format()
        .map_err(|_| error("image-format-invalid"))?
        .format()
        .ok_or_else(|| error("image-format-invalid"))?;
    format_identity(format)?;
    let mut limits = Limits::default();
    limits.max_image_width = Some(MAX_IMAGE_EDGE);
    limits.max_image_height = Some(MAX_IMAGE_EDGE);
    limits.max_alloc = Some(MAX_IMAGE_DECODE_ALLOC_BYTES);
    let mut reader = ImageReader::with_format(Cursor::new(bytes), format);
    reader.limits(limits);
    let image = reader.decode().map_err(|_| error("image-decode-failed"))?;
    Ok((image, format))
}

fn format_identity(format: ImageFormat) -> Result<(&'static str, &'static str), ImageContextError> {
    match format {
        ImageFormat::Png => Ok(("image/png", "png")),
        ImageFormat::Jpeg => Ok(("image/jpeg", "jpg")),
        ImageFormat::WebP => Ok(("image/webp", "webp")),
        _ => Err(error("image-format-unsupported")),
    }
}

fn validate_dimensions(width: u32, height: u32) -> Result<(), ImageContextError> {
    if width == 0
        || height == 0
        || width > MAX_IMAGE_EDGE
        || height > MAX_IMAGE_EDGE
        || u64::from(width).saturating_mul(u64::from(height)) > MAX_IMAGE_PIXELS
    {
        return Err(error("image-dimensions-invalid"));
    }
    Ok(())
}

fn error(code: &'static str) -> ImageContextError {
    ImageContextError { code }
}

#[cfg(test)]
mod tests {
    use super::*;
    use image::{ImageBuffer, Rgba};

    fn png(width: u32, height: u32) -> Vec<u8> {
        let image = DynamicImage::ImageRgba8(ImageBuffer::from_pixel(
            width,
            height,
            Rgba([24, 93, 255, 255]),
        ));
        let mut output = Cursor::new(Vec::new());
        image.write_to(&mut output, ImageFormat::Png).unwrap();
        output.into_inner()
    }

    #[test]
    fn validates_supported_image_and_creates_bounded_thumbnail() {
        let bytes = png(640, 480);
        let metadata = validate_image(&bytes, Some("image/png")).unwrap();
        assert_eq!(metadata.width, 640);
        assert_eq!(metadata.height, 480);
        assert_eq!(metadata.bytes, bytes.len() as u64);
        let thumbnail = thumbnail_png(&bytes, "image/png").unwrap();
        let thumbnail_metadata = validate_image(&thumbnail, Some("image/png")).unwrap();
        assert!(thumbnail_metadata.width <= THUMBNAIL_MAX_WIDTH);
        assert!(thumbnail_metadata.height <= THUMBNAIL_MAX_HEIGHT);
    }

    #[test]
    fn rejects_mismatched_unsupported_and_oversized_inputs() {
        let bytes = png(2, 2);
        assert_eq!(
            validate_image(&bytes, Some("image/jpeg")).unwrap_err().code,
            "image-media-type-mismatch"
        );
        assert_eq!(
            validate_image(b"not-an-image", None).unwrap_err().code,
            "image-format-invalid"
        );
        assert_eq!(
            validate_image(&vec![0; MAX_IMAGE_BYTES + 1], None)
                .unwrap_err()
                .code,
            "image-size-invalid"
        );
    }
}
