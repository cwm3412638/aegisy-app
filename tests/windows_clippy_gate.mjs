import { spawn, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

export const clippyArguments = Object.freeze([
  'clippy', '--locked', '--workspace', '--all-targets',
  '--manifest-path', 'agent-runtime/Cargo.toml', '--message-format=json',
  '--', '-D', 'warnings',
]);

const maxLineBytes = 1024 * 1024;
const maxDiagnostics = 20;
const annotationPrefix = '::error title=Windows Rust lint failure::';

export class ClippyDiagnostics {
  constructor(trackedFiles) {
    this.trackedFiles = trackedFiles;
    this.pending = Buffer.alloc(0);
    this.discarding = false;
    this.diagnostics = new Set();
  }

  acceptLine(bytes) {
    if (this.diagnostics.size >= maxDiagnostics) return;
    let frame;
    try { frame = JSON.parse(bytes.toString('utf8')); } catch { return; }
    if (frame?.reason !== 'compiler-message') return;
    const message = frame.message;
    if (!['error', 'warning'].includes(message?.level)) return;
    const code = message.code?.code;
    if (typeof code !== 'string' || !/^(?:E[0-9]{4}|(?:clippy::)?[a-z][a-z0-9_]{0,79})$/.test(code)) return;
    let location = 'external-or-unknown-source';
    if (Array.isArray(message.spans)) {
      for (const span of message.spans.slice(0, 256)) {
        if (span?.is_primary !== true || typeof span.file_name !== 'string') continue;
        const file = span.file_name.replaceAll('\\', '/');
        const candidate = file.startsWith('agent-runtime/') ? file : `agent-runtime/${file}`;
        if (candidate.length > 512 || !/^agent-runtime\/crates\/(?:[a-zA-Z0-9_-]+\/)*[a-zA-Z0-9_-]+\.rs$/.test(candidate)
            || !this.trackedFiles.has(candidate)) continue;
        if (![span.line_start, span.column_start].every(value => Number.isSafeInteger(value) && value > 0 && value <= 10000000)) continue;
        location = `${candidate}:${span.line_start}:${span.column_start}`;
        break;
      }
    }
    this.diagnostics.add(`${message.level} ${code} ${location}`);
  }

  push(chunk) {
    // Drain oversized physical lines without retaining or interpreting their tail.
    let start = 0;
    while (start < chunk.length) {
      const newline = chunk.indexOf(10, start);
      const end = newline < 0 ? chunk.length : newline;
      const segment = chunk.subarray(start, end);
      if (!this.discarding) {
        if (this.pending.length + segment.length > maxLineBytes) {
          this.pending = Buffer.alloc(0);
          this.discarding = true;
        } else {
          this.pending = Buffer.concat([this.pending, segment]);
        }
      }
      if (newline < 0) break;
      if (!this.discarding) this.acceptLine(this.pending);
      this.pending = Buffer.alloc(0);
      this.discarding = false;
      start = newline + 1;
    }
  }

  annotation() {
    if (!this.discarding && this.pending.length) this.acceptLine(this.pending);
    const lines = [...this.diagnostics];
    if (!lines.length) lines.push('CLIPPY_FAILED_WITHOUT_CLASSIFIED_DIAGNOSTIC');
    let encoded = '';
    for (const line of lines) {
      const next = line.replaceAll('%', '%25').replaceAll('\r', '%0D').replaceAll('\n', '%0A');
      if (encoded.length + next.length + (encoded ? 3 : 0) > 2000) break;
      encoded += (encoded ? '%0A' : '') + next;
    }
    return annotationPrefix + encoded;
  }
}

export async function runClippy(trackedFiles, spawnProcess = spawn, publish = console.log) {
  const diagnostics = new ClippyDiagnostics(trackedFiles);
  return await new Promise(resolve => {
    let child;
    try {
      child = spawnProcess(process.platform === 'win32' ? 'cargo.exe' : 'cargo', clippyArguments,
        { shell: false, stdio: ['ignore', 'pipe', 'pipe'] });
    } catch {
      publish(annotationPrefix + 'CLIPPY_PROCESS_UNAVAILABLE');
      resolve(1);
      return;
    }
    child.stdout.on('data', chunk => diagnostics.push(chunk));
    child.stderr.resume();
    let unavailable = false;
    child.on('error', () => { unavailable = true; });
    child.on('close', (code, signal) => {
      const exitCode = !unavailable && !signal && Number.isInteger(code) && code >= 0 && code <= 255 ? code : 1;
      if (unavailable) publish(annotationPrefix + 'CLIPPY_PROCESS_UNAVAILABLE');
      else if (exitCode !== 0) publish(diagnostics.annotation());
      resolve(exitCode);
    });
  });
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  // Only tracked repository filenames may reach a public diagnostic location.
  const inventory = spawnSync('git', ['ls-files', '-z', '--', 'agent-runtime/crates'],
    { shell: false, encoding: 'utf8', maxBuffer: 1024 * 1024 });
  if (inventory.status !== 0 || inventory.error) {
    console.log(annotationPrefix + 'CLIPPY_SOURCE_INVENTORY_UNAVAILABLE');
    process.exitCode = 1;
  } else {
    process.exitCode = await runClippy(new Set(inventory.stdout.split('\0')));
  }
}
