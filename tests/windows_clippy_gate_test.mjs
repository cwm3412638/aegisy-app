import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { test } from 'node:test';
import { ClippyDiagnostics, clippyArguments, runClippy } from './windows_clippy_gate.mjs';

const file = 'agent-runtime/crates/aegisy-agentd/src/lib.rs';
const files = new Set([file]);
const diagnostic = (overrides = {}) => JSON.stringify({
  reason: 'compiler-message',
  message: { level: 'error', code: { code: 'clippy::large_enum_variant' },
    message: 'private body', rendered: 'private source', children: [{ message: 'private suggestion' }],
    spans: [{ is_primary: true, file_name: 'crates/aegisy-agentd/src/lib.rs', line_start: 42, column_start: 3,
      text: [{ text: 'private source' }] }], ...overrides },
});

test('structured diagnostics expose only code and tracked location across chunk boundaries', () => {
  const collector = new ClippyDiagnostics(files);
  const bytes = Buffer.from(diagnostic() + '\r\n');
  for (const byte of bytes) collector.push(Buffer.from([byte]));
  assert.equal(collector.annotation(), '::error title=Windows Rust lint failure::error clippy::large_enum_variant ' + file + ':42:3');
});

test('malformed, unclassified, injected, and external source data stay private', () => {
  for (const source of ['/private/home/person/file.rs', '../secret.rs', 'crates/unknown/private.rs', 'crates/aegisy-agentd/src/lib.rs\n::error::private']) {
    const collector = new ClippyDiagnostics(files);
    collector.push(Buffer.from(diagnostic({ spans: [{ is_primary: true, file_name: source, line_start: 1, column_start: 1 }] })));
    assert.equal(collector.annotation(), '::error title=Windows Rust lint failure::error clippy::large_enum_variant external-or-unknown-source');
  }
  const collector = new ClippyDiagnostics(files);
  for (const line of ['not json', '{', JSON.stringify({ reason: 'build-script-executed', message: 'private' }),
    diagnostic({ level: 'note' }), diagnostic({ code: { code: 'private\n::error::payload' } }), diagnostic({ code: null })]) {
    collector.push(Buffer.from(line + '\n'));
  }
  assert.match(collector.annotation(), /::CLIPPY_FAILED_WITHOUT_CLASSIFIED_DIAGNOSTIC$/);
});

test('Windows separators normalize but unsafe positions are not published', () => {
  const collector = new ClippyDiagnostics(files);
  collector.push(Buffer.from(diagnostic({ code: { code: 'E0308' }, spans: [
    { is_primary: true, file_name: file.replaceAll('/', '\\'), line_start: 23, column_start: 4 },
  ] })));
  assert.match(collector.annotation(), /error E0308 agent-runtime\/crates\/aegisy-agentd\/src\/lib.rs:23:4$/);
  for (const value of [0, -1, 1.5, '23', 10000001]) {
    const invalid = new ClippyDiagnostics(files);
    invalid.push(Buffer.from(diagnostic({ spans: [{ is_primary: true, file_name: file, line_start: value, column_start: 1 }] })));
    assert.match(invalid.annotation(), /external-or-unknown-source$/);
  }
});

test('oversized lines drain fully, duplicate/count/output bounds hold, and EOF is handled', () => {
  const collector = new ClippyDiagnostics(files);
  collector.push(Buffer.alloc(1024 * 1024 + 1, 32));
  collector.push(Buffer.from(diagnostic() + '\n'));
  assert.equal(collector.diagnostics.size, 0);
  assert.equal(collector.pending.length, 0);
  for (let index = 0; index < 100; index++) {
    collector.push(Buffer.from(diagnostic({ code: { code: `clippy::lint_${index}` } }) + '\n'));
  }
  assert.equal(collector.diagnostics.size, 20);
  assert.ok(collector.annotation().slice('::error title=Windows Rust lint failure::'.length).length <= 2000);
  const eof = new ClippyDiagnostics(files);
  eof.push(Buffer.from(diagnostic()));
  assert.match(eof.annotation(), /large_enum_variant/);
  eof.push(Buffer.from('\n' + diagnostic() + '\n'));
  assert.equal(eof.diagnostics.size, 1);
});

test('long tracked paths cannot produce an empty or oversized annotation', () => {
  const longFile = 'agent-runtime/crates/' + 'a'.repeat(450) + '.rs';
  const tooLong = 'agent-runtime/crates/' + 'a'.repeat(2100) + '.rs';
  const collector = new ClippyDiagnostics(new Set([longFile, tooLong]));
  for (let line = 1; line <= 20; line++) {
    collector.push(Buffer.from(diagnostic({ spans: [{ is_primary: true, file_name: longFile, line_start: line, column_start: 1 }] }) + '\n'));
  }
  const body = collector.annotation().slice('::error title=Windows Rust lint failure::'.length);
  assert.ok(body.length > 1500 && body.length <= 2000);
  assert.ok(body.endsWith(':1'));
  const oversized = new ClippyDiagnostics(new Set([tooLong]));
  oversized.push(Buffer.from(diagnostic({ spans: [{ is_primary: true, file_name: tooLong, line_start: 1, column_start: 1 }] })));
  assert.match(oversized.annotation(), /external-or-unknown-source$/);
});

test('real child completion preserves failure and success; stdout/stderr are never forwarded', async () => {
  for (const code of [0, 7, 101]) {
    const published = [];
    const result = await runClippy(files, (command, args, options) => {
      assert.equal(command, process.platform === 'win32' ? 'cargo.exe' : 'cargo');
      assert.deepEqual(args, clippyArguments);
      assert.deepEqual(args, ['clippy', '--locked', '--workspace', '--all-targets', '--manifest-path', 'agent-runtime/Cargo.toml', '--message-format=json', '--', '-D', 'warnings']);
      assert.equal(options.shell, false);
      return spawn(process.execPath, ['-e', `process.stdout.write(${JSON.stringify(diagnostic())}); process.stderr.write('private stderr'); process.exitCode = ${code};`], options);
    }, line => published.push(line));
    assert.equal(result, code);
    assert.equal(published.length, code ? 1 : 0);
    assert.ok(published.every(line => !line.includes('private')));
  }
});

test('missing executable and abnormal termination fail closed with fixed diagnostics', async () => {
  const published = [];
  assert.equal(await runClippy(files, (_command, _args, options) => spawn('aegisy-no-such-clippy-executable', [], options), line => published.push(line)), 1);
  assert.deepEqual(published, ['::error title=Windows Rust lint failure::CLIPPY_PROCESS_UNAVAILABLE']);
  const terminated = [];
  assert.equal(await runClippy(files, (_command, _args, options) => spawn(process.execPath,
    ['-e', "process.kill(process.pid, 'SIGTERM')"], options), line => terminated.push(line)), 1);
  assert.equal(terminated.length, 1);
});
