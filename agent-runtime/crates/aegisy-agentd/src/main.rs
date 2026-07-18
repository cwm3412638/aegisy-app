use aegisy_agentd::Runtime;
use serde_json::{json, Value};
use std::io::{self, BufRead, Write};
use std::path::Path;
use std::sync::mpsc::TrySendError;
use std::sync::{mpsc, Arc, Mutex};
use std::thread;

const REQUEST_QUEUE_CAPACITY: usize = 32;

fn write_message(stdout: &Arc<Mutex<io::Stdout>>, message: &Value) -> bool {
    let Ok(mut encoded) = serde_json::to_vec(message) else {
        return false;
    };
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
        for line in io::stdin().lock().lines().map_while(Result::ok) {
            if line.trim().is_empty() {
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
                    let Ok(request) = serde_json::from_str::<aegisy_aap::Request>(&line) else {
                        continue;
                    };
                    let Some(id) = request.id else {
                        continue;
                    };
                    let overload = json!({
                        "jsonrpc": "2.0",
                        "id": id,
                        "error": {
                            "code": -32004,
                            "message": "AAP request queue is full"
                        }
                    });
                    if !write_message(&reader_stdout, &overload) {
                        return;
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
