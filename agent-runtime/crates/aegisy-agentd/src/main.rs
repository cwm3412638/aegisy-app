use aegisy_aap::MAX_AAP_FRAME_BYTES;
use aegisy_agentd::Runtime;
use serde_json::{json, Value};
use std::io::{self, BufRead, Write};
use std::path::Path;
use std::sync::mpsc::TrySendError;
use std::sync::{mpsc, Arc, Mutex};
use std::thread;

const REQUEST_QUEUE_CAPACITY: usize = 32;

enum FrameRead {
    Frame(Vec<u8>),
    Oversized,
}

fn read_bounded_frame<R: BufRead>(reader: &mut R) -> io::Result<Option<FrameRead>> {
    let mut frame = Vec::new();
    let mut oversized = false;
    loop {
        let available = reader.fill_buf()?;
        if available.is_empty() {
            if frame.is_empty() && !oversized {
                return Ok(None);
            }
            let over_limit_at_eof = frame.len()
                > usize::try_from(MAX_AAP_FRAME_BYTES).expect("AAP frame limit fits usize");
            return Ok(Some(if oversized || over_limit_at_eof {
                FrameRead::Oversized
            } else {
                FrameRead::Frame(frame)
            }));
        }
        let newline = available.iter().position(|byte| *byte == b'\n');
        let consumed = newline.map_or(available.len(), |position| position + 1);
        let payload = newline.map_or(available, |position| &available[..position]);
        if !oversized {
            let limit = usize::try_from(MAX_AAP_FRAME_BYTES).expect("AAP frame limit fits usize");
            let next_len = frame.len().saturating_add(payload.len());
            if next_len > limit.saturating_add(1) {
                oversized = true;
                frame.clear();
            } else {
                frame.extend_from_slice(payload);
                if frame.len() == limit.saturating_add(1) && frame.last() != Some(&b'\r') {
                    oversized = true;
                    frame.clear();
                }
            }
        }
        reader.consume(consumed);
        if newline.is_some() {
            if frame.last() == Some(&b'\r') {
                frame.pop();
            }
            if frame.len()
                > usize::try_from(MAX_AAP_FRAME_BYTES).expect("AAP frame limit fits usize")
            {
                oversized = true;
                frame.clear();
            }
            return Ok(Some(if oversized {
                FrameRead::Oversized
            } else {
                FrameRead::Frame(frame)
            }));
        }
    }
}

fn oversized_frame_error() -> Value {
    json!({
        "jsonrpc": "2.0",
        "id": null,
        "error": {
            "code": -32005,
            "message": "AAP frame exceeds the negotiated limit"
        }
    })
}

fn write_message<W: Write>(stdout: &Arc<Mutex<W>>, message: &Value) -> bool {
    let Ok(mut encoded) = serde_json::to_vec(message) else {
        return false;
    };
    if encoded.len() > usize::try_from(MAX_AAP_FRAME_BYTES).expect("AAP frame limit fits usize") {
        let Some(id) = message.as_object().and_then(|object| object.get("id")) else {
            return false;
        };
        let fallback = json!({
            "jsonrpc": "2.0",
            "id": id,
            "error": {
                "code": -32005,
                "message": "AAP response exceeds the negotiated frame limit"
            }
        });
        let Ok(fallback) = serde_json::to_vec(&fallback) else {
            return false;
        };
        encoded = fallback;
    }
    encoded.push(b'\n');
    let Ok(mut stdout) = stdout.lock() else {
        return false;
    };
    stdout.write_all(&encoded).is_ok() && stdout.flush().is_ok()
}

fn main() {
    let preview = std::env::var("AEGISY_AGENT_BACKEND").as_deref() == Ok("preview");
    let mut runtime = match std::env::var_os("AEGISY_WORKBENCH_DATA_ROOT") {
        Some(data_root) => {
            let data_root = Path::new(&data_root);
            let result = if preview {
                Runtime::with_store(data_root)
            } else {
                Runtime::with_codex_and_store(data_root)
            };
            result.unwrap_or_else(|error| {
                eprintln!("Aegisy workbench store is unavailable: {error}");
                Runtime::unavailable(error)
            })
        }
        None if preview => Runtime::default(),
        None => Runtime::with_codex().unwrap_or_else(|error| {
            eprintln!("Codex App Server is unavailable: {error}");
            Runtime::unavailable(error)
        }),
    };
    let control = runtime.control();
    let stdout = Arc::new(Mutex::new(io::stdout()));
    let reader_stdout = stdout.clone();
    let (request_sender, request_receiver) = mpsc::sync_channel(REQUEST_QUEUE_CAPACITY);

    thread::spawn(move || {
        let mut stdin = io::stdin().lock();
        while let Ok(Some(frame)) = read_bounded_frame(&mut stdin) {
            let FrameRead::Frame(frame) = frame else {
                if !write_message(&reader_stdout, &oversized_frame_error()) {
                    return;
                }
                continue;
            };
            if frame.iter().all(|byte| byte.is_ascii_whitespace()) {
                continue;
            }
            let line = match String::from_utf8(frame) {
                Ok(line) => line,
                Err(_) => {
                    let parse_error = json!({
                        "jsonrpc": "2.0",
                        "id": null,
                        "error": {"code": -32700, "message": "parse error: invalid UTF-8"}
                    });
                    if !write_message(&reader_stdout, &parse_error) {
                        return;
                    }
                    continue;
                }
            };
            if let Some(messages) = control.reject_oversized_line(&line) {
                for message in messages {
                    if !write_message(&reader_stdout, &message) {
                        return;
                    }
                }
                continue;
            }
            if let Some(messages) = control.handle_out_of_band_line(&line) {
                for message in messages {
                    if !write_message(&reader_stdout, &message) {
                        return;
                    }
                }
                continue;
            }
            match request_sender.try_send(line) {
                Ok(()) => {}
                Err(TrySendError::Full(line)) => {
                    if let Some(overload) = control.overload_response(&line) {
                        if !write_message(&reader_stdout, &overload) {
                            return;
                        }
                    }
                }
                Err(TrySendError::Disconnected(_)) => return,
            }
        }
    });

    while let Ok(line) = request_receiver.recv() {
        let mut output_open = true;
        runtime.handle_line_stream(&line, |message| {
            if output_open {
                output_open = write_message(&stdout, &message);
            }
        });
        if !output_open {
            return;
        }
        if runtime.should_shutdown() {
            break;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{oversized_frame_error, read_bounded_frame, write_message, FrameRead};
    use aegisy_aap::MAX_AAP_FRAME_BYTES;
    use std::io::{BufReader, Cursor};
    use std::sync::{Arc, Mutex};

    #[test]
    fn bounded_reader_drains_an_oversized_frame_and_recovers_next_frame() {
        let mut bytes = vec![b'x'; usize::try_from(MAX_AAP_FRAME_BYTES).unwrap() + 1];
        bytes.extend_from_slice(b"\n{\"jsonrpc\":\"2.0\"}\n");
        let mut reader = BufReader::with_capacity(1024, Cursor::new(bytes));
        assert!(matches!(
            read_bounded_frame(&mut reader).unwrap(),
            Some(FrameRead::Oversized)
        ));
        let Some(FrameRead::Frame(frame)) = read_bounded_frame(&mut reader).unwrap() else {
            panic!("next frame should remain readable");
        };
        assert_eq!(frame, br#"{"jsonrpc":"2.0"}"#);
    }

    #[test]
    fn bounded_reader_accepts_exact_limit_and_strips_crlf() {
        let mut bytes = vec![b'a'; usize::try_from(MAX_AAP_FRAME_BYTES).unwrap()];
        bytes.extend_from_slice(b"\r\n");
        let mut reader = BufReader::with_capacity(4096, Cursor::new(bytes));
        let Some(FrameRead::Frame(frame)) = read_bounded_frame(&mut reader).unwrap() else {
            panic!("exact-limit frame should be retained");
        };
        assert_eq!(frame.len(), usize::try_from(MAX_AAP_FRAME_BYTES).unwrap());
    }

    #[test]
    fn bounded_reader_rejects_limit_plus_cr_at_eof_without_lf() {
        let mut bytes = vec![b'a'; usize::try_from(MAX_AAP_FRAME_BYTES).unwrap()];
        bytes.push(b'\r');
        let mut reader = BufReader::with_capacity(4096, Cursor::new(bytes));
        assert!(matches!(
            read_bounded_frame(&mut reader).unwrap(),
            Some(FrameRead::Oversized)
        ));
    }

    #[test]
    fn oversized_ingress_error_is_fixed_content_free_and_bounded() {
        let error = oversized_frame_error();
        assert_eq!(error["jsonrpc"], "2.0");
        assert!(error["id"].is_null());
        assert_eq!(error["error"]["code"], -32005);
        assert!(serde_json::to_vec(&error).unwrap().len() < 256);
    }

    #[test]
    fn outbound_writer_replaces_oversized_responses_and_remains_usable() {
        let output = Arc::new(Mutex::new(Vec::new()));
        let oversized = serde_json::json!({
            "jsonrpc": "2.0",
            "id": "oversized-response",
            "result": {"data": "x".repeat(usize::try_from(MAX_AAP_FRAME_BYTES).unwrap())}
        });
        assert!(write_message(&output, &oversized));
        assert!(write_message(
            &output,
            &serde_json::json!({"jsonrpc": "2.0", "id": "next", "result": {}})
        ));

        let bytes = output.lock().unwrap().clone();
        let lines = bytes
            .split(|byte| *byte == b'\n')
            .filter(|line| !line.is_empty())
            .map(|line| serde_json::from_slice::<serde_json::Value>(line).unwrap())
            .collect::<Vec<_>>();
        assert_eq!(lines.len(), 2);
        assert_eq!(lines[0]["id"], "oversized-response");
        assert_eq!(lines[0]["error"]["code"], -32005);
        assert_eq!(lines[1]["id"], "next");
        assert!(bytes.len() < 1024);
    }

    #[test]
    fn outbound_writer_serializes_complete_frames_across_concurrent_producers() {
        const PRODUCERS: usize = 8;
        const MESSAGES_PER_PRODUCER: usize = 64;
        let output = Arc::new(Mutex::new(Vec::new()));
        let producers = (0..PRODUCERS)
            .map(|producer| {
                let output = output.clone();
                std::thread::spawn(move || {
                    for sequence in 0..MESSAGES_PER_PRODUCER {
                        assert!(write_message(
                            &output,
                            &serde_json::json!({
                                "jsonrpc": "2.0",
                                "id": format!("producer-{producer}-{sequence}"),
                                "result": {
                                    "schema_version": "runtime-heartbeat/0.1",
                                    "nonce": format!("nonce-{producer}-{sequence}"),
                                    "state": "alive"
                                }
                            })
                        ));
                    }
                })
            })
            .collect::<Vec<_>>();
        for producer in producers {
            producer.join().unwrap();
        }

        let bytes = output.lock().unwrap().clone();
        let frames = bytes
            .split(|byte| *byte == b'\n')
            .filter(|line| !line.is_empty())
            .map(|line| serde_json::from_slice::<serde_json::Value>(line).unwrap())
            .collect::<Vec<_>>();
        assert_eq!(frames.len(), PRODUCERS * MESSAGES_PER_PRODUCER);
        let ids = frames
            .iter()
            .map(|frame| frame["id"].as_str().unwrap())
            .collect::<std::collections::HashSet<_>>();
        assert_eq!(ids.len(), frames.len());
        assert!(frames.iter().all(|frame| {
            frame["result"]["schema_version"] == "runtime-heartbeat/0.1"
                && frame["result"]["state"] == "alive"
        }));
    }

    #[test]
    fn outbound_writer_fails_closed_on_oversized_notifications() {
        let output = Arc::new(Mutex::new(Vec::new()));
        let oversized = serde_json::json!({
            "jsonrpc": "2.0",
            "method": "timeline/event",
            "params": {"data": "x".repeat(usize::try_from(MAX_AAP_FRAME_BYTES).unwrap())}
        });
        assert!(!write_message(&output, &oversized));
        assert!(output.lock().unwrap().is_empty());
    }
}
