use aegisy_aap::MAX_AAP_FRAME_BYTES;
use aegisy_agentd::{
    decode_request_frame, request_frame_error_response, DecodedRequestFrame, RequestFrameError,
    Runtime,
};
use serde_json::{json, Value};
use std::io::{self, BufRead, Write};
use std::path::Path;
use std::sync::mpsc::TrySendError;
use std::sync::{mpsc, Arc, Mutex};
use std::thread;

const REQUEST_QUEUE_CAPACITY: usize = 32;

#[derive(Default)]
struct TransportFaultGate {
    failed: Mutex<bool>,
}

impl TransportFaultGate {
    fn fail(&self) {
        let mut failed = self
            .failed
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        *failed = true;
    }

    fn dispatch<T>(&self, operation: impl FnOnce() -> T) -> Option<T> {
        let failed = self.failed.lock().ok()?;
        if *failed {
            return None;
        }
        Some(operation())
    }
}

fn fail_closed_on_validator_unavailable<T>(
    result: Result<T, RequestFrameError>,
    gate: &TransportFaultGate,
) -> Result<T, RequestFrameError> {
    if matches!(&result, Err(RequestFrameError::ValidatorUnavailable)) {
        gate.fail();
    }
    result
}

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
    let emergency_disabled =
        std::env::var("AEGISY_WORKBENCH_EMERGENCY_DISABLED").as_deref() == Ok("1");
    let mut runtime = match std::env::var_os("AEGISY_WORKBENCH_DATA_ROOT") {
        Some(data_root) => {
            let data_root = Path::new(&data_root);
            let result = if emergency_disabled {
                Runtime::with_emergency_store(data_root)
            } else if preview {
                Runtime::with_store(data_root)
            } else {
                Runtime::with_codex_and_store(data_root)
            };
            result.unwrap_or_else(|error| {
                eprintln!("Aegisy workbench store is unavailable: {error}");
                if emergency_disabled {
                    Runtime::emergency_unavailable(error)
                } else {
                    Runtime::unavailable(error)
                }
            })
        }
        None if emergency_disabled => Runtime::emergency(),
        None if preview => Runtime::default(),
        None => Runtime::with_codex().unwrap_or_else(|error| {
            eprintln!("Codex App Server is unavailable: {error}");
            Runtime::unavailable(error)
        }),
    };
    let control = runtime.control();
    let reader_control = control.clone();
    let stdout = Arc::new(Mutex::new(io::stdout()));
    let reader_stdout = stdout.clone();
    let transport_fault_gate = Arc::new(TransportFaultGate::default());
    let reader_transport_fault_gate = Arc::clone(&transport_fault_gate);
    let (request_sender, request_receiver): (
        mpsc::SyncSender<DecodedRequestFrame>,
        mpsc::Receiver<DecodedRequestFrame>,
    ) = mpsc::sync_channel(REQUEST_QUEUE_CAPACITY);

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
            let decoded = match fail_closed_on_validator_unavailable(
                decode_request_frame(&frame),
                &reader_transport_fault_gate,
            ) {
                Ok(decoded) => decoded,
                Err(RequestFrameError::ValidatorUnavailable) => return,
                Err(error) => {
                    if let Some(response) = request_frame_error_response(&error) {
                        if !write_message(&reader_stdout, &response) {
                            return;
                        }
                    }
                    continue;
                }
            };
            if let Some(messages) = reader_control.reject_oversized_frame(&decoded) {
                for message in messages {
                    if !write_message(&reader_stdout, &message) {
                        return;
                    }
                }
                continue;
            }
            match reader_control.handle_out_of_band_frame(&decoded) {
                Ok(Some(messages)) => {
                    for message in messages {
                        if !write_message(&reader_stdout, &message) {
                            return;
                        }
                    }
                    continue;
                }
                Ok(None) => {}
                Err(RequestFrameError::ValidatorUnavailable) => {
                    reader_transport_fault_gate.fail();
                    return;
                }
                Err(_) => return,
            }
            match request_sender.try_send(decoded) {
                Ok(()) => {}
                Err(TrySendError::Full(frame)) => {
                    if let Some(overload) = reader_control.overload_response_frame(&frame) {
                        if !write_message(&reader_stdout, &overload) {
                            return;
                        }
                    }
                }
                Err(TrySendError::Disconnected(_)) => return,
            }
        }
    });

    while let Ok(frame) = request_receiver.recv() {
        let Some((output_open, should_shutdown)) = transport_fault_gate.dispatch(|| {
            let mut output_open = true;
            match control.handle_out_of_band_frame(&frame) {
                Ok(Some(messages)) => {
                    for message in messages {
                        if output_open {
                            output_open = write_message(&stdout, &message);
                        }
                    }
                }
                Ok(None) => runtime.handle_frame_stream(frame, |message| {
                    if output_open {
                        output_open = write_message(&stdout, &message);
                    }
                }),
                Err(RequestFrameError::ValidatorUnavailable) => return (false, true),
                Err(_) => return (false, true),
            }
            (output_open, runtime.should_shutdown())
        }) else {
            break;
        };
        if !output_open {
            return;
        }
        if should_shutdown {
            break;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{
        fail_closed_on_validator_unavailable, oversized_frame_error, read_bounded_frame,
        write_message, FrameRead, TransportFaultGate,
    };
    use aegisy_aap::MAX_AAP_FRAME_BYTES;
    use aegisy_agentd::RequestFrameError;
    use std::io::{BufReader, Cursor};
    use std::sync::{mpsc, Arc, Mutex};
    use std::thread;

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
    fn transport_fault_gate_linearizes_failure_before_later_dispatch() {
        let gate = Arc::new(TransportFaultGate::default());
        let (entered_sender, entered_receiver) = mpsc::channel();
        let (release_sender, release_receiver) = mpsc::channel();
        let active_gate = Arc::clone(&gate);
        let active = thread::spawn(move || {
            active_gate.dispatch(|| {
                entered_sender.send(()).unwrap();
                release_receiver.recv().unwrap();
            })
        });
        entered_receiver.recv().unwrap();
        assert!(gate.failed.try_lock().is_err());

        let fault_gate = Arc::clone(&gate);
        let fault = thread::spawn(move || fault_gate.fail());
        release_sender.send(()).unwrap();
        assert!(active.join().unwrap().is_some());
        fault.join().unwrap();

        let mut later_dispatched = false;
        assert!(gate
            .dispatch(|| {
                later_dispatched = true;
            })
            .is_none());
        assert!(!later_dispatched);
    }

    #[test]
    fn transport_fault_gate_blocks_dispatch_when_failure_wins() {
        let gate = TransportFaultGate::default();
        gate.fail();
        let mut dispatched = false;
        assert!(gate
            .dispatch(|| {
                dispatched = true;
            })
            .is_none());
        assert!(!dispatched);
    }

    #[test]
    fn validator_unavailable_fails_gate_before_queued_dispatch() {
        let gate = TransportFaultGate::default();
        let failure: Result<(), RequestFrameError> = Err(RequestFrameError::ValidatorUnavailable);
        assert_eq!(
            fail_closed_on_validator_unavailable(failure, &gate),
            Err(RequestFrameError::ValidatorUnavailable)
        );

        let mut dispatched = false;
        assert!(gate
            .dispatch(|| {
                dispatched = true;
            })
            .is_none());
        assert!(!dispatched);
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
