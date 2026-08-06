use aegisy_aap::MAX_AAP_FRAME_BYTES;
use aegisy_agentd::bootstrap_auth::{bootstrap_auth_error_response, BootstrapToken};
use aegisy_agentd::{
    decode_request_frame, request_frame_error_response, DecodedRequestFrame, RequestFrameError,
    Runtime,
};
use serde_json::{json, Value};
use std::io::{self, BufRead, BufReader, Read, Write};
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

fn create_runtime() -> Runtime {
    let preview = std::env::var("AEGISY_AGENT_BACKEND").as_deref() == Ok("preview");
    let emergency_disabled =
        std::env::var("AEGISY_WORKBENCH_EMERGENCY_DISABLED").as_deref() == Ok("1");
    match std::env::var_os("AEGISY_WORKBENCH_DATA_ROOT") {
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
    }
}

fn serve_connection<R, W>(
    mut runtime: Runtime,
    reader: R,
    writer: W,
    bootstrap: Option<BootstrapToken>,
    progress_marker: Option<&'static str>,
) where
    R: Read + Send + 'static,
    W: Write + Send + 'static,
{
    let mut input = BufReader::new(reader);
    if let Some(mut token) = bootstrap {
        // A token-configured sidecar requires the exact one-time bootstrap
        // prelude as the first line before any AAP frame is processed. A
        // missing, malformed, replayed, or mismatched prelude fails closed
        // with a fixed content-free error and no Runtime/Store/adapter state
        // is constructed from client input.
        let verified = match read_bounded_frame(&mut input) {
            Ok(Some(FrameRead::Frame(frame))) => token.verify_prelude_frame(&frame).is_ok(),
            _ => false,
        };
        if !verified {
            let output = Arc::new(Mutex::new(writer));
            write_message(&output, &bootstrap_auth_error_response());
            return;
        }
        if runtime.mark_bootstrap_authenticated().is_err() {
            let output = Arc::new(Mutex::new(writer));
            write_message(&output, &bootstrap_auth_error_response());
            return;
        }
        if let Some(marker) = progress_marker {
            eprintln!("Aegisy {marker}: bootstrap-accepted");
        }
    }
    let control = runtime.control();
    let reader_control = control.clone();
    let output = Arc::new(Mutex::new(writer));
    let reader_output = output.clone();
    let transport_fault_gate = Arc::new(TransportFaultGate::default());
    let reader_transport_fault_gate = Arc::clone(&transport_fault_gate);
    let (request_sender, request_receiver): (
        mpsc::SyncSender<DecodedRequestFrame>,
        mpsc::Receiver<DecodedRequestFrame>,
    ) = mpsc::sync_channel(REQUEST_QUEUE_CAPACITY);

    thread::spawn(move || {
        let mut input = input;
        while let Ok(Some(frame)) = read_bounded_frame(&mut input) {
            let FrameRead::Frame(frame) = frame else {
                if !write_message(&reader_output, &oversized_frame_error()) {
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
                        if !write_message(&reader_output, &response) {
                            return;
                        }
                    }
                    continue;
                }
            };
            if let Some(messages) = reader_control.reject_oversized_frame(&decoded) {
                for message in messages {
                    if !write_message(&reader_output, &message) {
                        return;
                    }
                }
                continue;
            }
            match reader_control.handle_out_of_band_frame(&decoded) {
                Ok(Some(messages)) => {
                    for message in messages {
                        if !write_message(&reader_output, &message) {
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
                        if !write_message(&reader_output, &overload) {
                            return;
                        }
                    }
                }
                Err(TrySendError::Disconnected(_)) => return,
            }
        }
    });

    let mut first_dispatch_marked = false;
    let mut first_frame_done_marked = false;
    while let Ok(frame) = request_receiver.recv() {
        if !first_dispatch_marked {
            first_dispatch_marked = true;
            if let Some(marker) = progress_marker {
                eprintln!("Aegisy {marker}: first-frame-dispatch");
            }
        }
        let Some((output_open, should_shutdown)) = transport_fault_gate.dispatch(|| {
            let mut output_open = true;
            match control.handle_out_of_band_frame(&frame) {
                Ok(Some(messages)) => {
                    for message in messages {
                        if output_open {
                            output_open = write_message(&output, &message);
                        }
                    }
                }
                Ok(None) => runtime.handle_frame_stream(frame, |message| {
                    if output_open {
                        output_open = write_message(&output, &message);
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
        if !first_frame_done_marked {
            first_frame_done_marked = true;
            if let Some(marker) = progress_marker {
                eprintln!("Aegisy {marker}: first-frame-done output-open");
            }
        }
        if should_shutdown {
            break;
        }
    }
    if let Some(marker) = progress_marker {
        eprintln!("Aegisy {marker}: serve-exit");
    }
}

fn main() {
    // Read and clear the one-time bootstrap token before constructing any
    // Runtime, adapter, terminal, or Git child so no descendant process can
    // inherit it. A present but malformed token fails startup closed.
    let bootstrap = match BootstrapToken::from_environment() {
        Ok(bootstrap) => bootstrap,
        Err(error) => {
            eprintln!(
                "Aegisy bootstrap authentication unavailable: {}",
                error.code()
            );
            return;
        }
    };

    #[cfg(target_os = "macos")]
    if let Some(directory) = std::env::var_os("AEGISY_AGENTD_UNIX_SOCKET_DIR") {
        use aegisy_agentd::macos_unix_socket::OwnerOnlyUnixListener;

        let listener = match OwnerOnlyUnixListener::bind_fresh(Path::new(&directory), unsafe {
            libc::getppid()
        }) {
            Ok(listener) => listener,
            Err(error) => {
                eprintln!("Aegisy Unix transport unavailable: {}", error.code());
                return;
            }
        };
        let stream = match listener.accept_one() {
            Ok(stream) => stream,
            Err(error) => {
                eprintln!("Aegisy Unix transport rejected: {}", error.code());
                return;
            }
        };
        let reader = match stream.try_clone() {
            Ok(reader) => reader,
            Err(_) => {
                eprintln!("Aegisy Unix transport unavailable: unix-socket-clone-failed");
                return;
            }
        };
        let mut runtime = create_runtime();
        if runtime
            .bind_verified_unix_socket_transport(&stream)
            .is_err()
        {
            eprintln!("Aegisy Unix transport unavailable: unix-socket-runtime-bind-failed");
            return;
        }
        serve_connection(runtime, reader, stream, bootstrap, None);
        return;
    }

    #[cfg(target_os = "windows")]
    if let Some(name) = std::env::var_os("AEGISY_AGENTD_NAMED_PIPE") {
        use aegisy_agentd::windows_named_pipe::{
            current_parent_process_identity, OwnerOnlyNamedPipeListener,
        };

        let Some(name) = name.to_str() else {
            eprintln!("Aegisy Windows transport unavailable: windows-named-pipe-invalid-name");
            return;
        };
        let expected_parent = match current_parent_process_identity() {
            Ok(pid) => pid,
            Err(error) => {
                eprintln!("Aegisy Windows transport unavailable: {error}");
                return;
            }
        };
        let listener =
            match OwnerOnlyNamedPipeListener::bind_fresh_with_identity(name, expected_parent) {
                Ok(listener) => listener,
                Err(error) => {
                    eprintln!("Aegisy Windows transport unavailable: {error}");
                    return;
                }
            };
        eprintln!("Aegisy Windows transport: pipe-bound");
        let connection = match listener.accept_one() {
            Ok(connection) => connection,
            Err(error) => {
                eprintln!("Aegisy Windows transport rejected: {error}");
                return;
            }
        };
        eprintln!("Aegisy Windows transport: pipe-accepted");
        let reader = match connection.try_clone() {
            Ok(reader) => reader,
            Err(_) => {
                eprintln!("Aegisy Windows transport unavailable: windows-named-pipe-clone-failed");
                return;
            }
        };
        let mut runtime = create_runtime();
        if runtime
            .bind_verified_named_pipe_transport(&connection)
            .is_err()
        {
            eprintln!(
                "Aegisy Windows transport unavailable: windows-named-pipe-runtime-bind-failed"
            );
            return;
        }
        serve_connection(
            runtime,
            reader,
            connection,
            bootstrap,
            Some("Windows transport"),
        );
        return;
    }

    serve_connection(create_runtime(), io::stdin(), io::stdout(), bootstrap, None);
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
