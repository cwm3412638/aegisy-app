'use strict';

const assert = require('assert');
const http = require('http');
const { spawn } = require('child_process');
const path = require('path');

const token = 'local-test-token';
const upstreamKey = 'upstream-secret-key';

function listen(server) {
  return new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', () => resolve(server.address().port));
  });
}

function request(port, route) {
  return new Promise(resolve => {
    const req = http.request({
      hostname: '127.0.0.1',
      port,
      method: 'POST',
      path: `/tools/codex${route}`,
      headers: {
        authorization: `Bearer ${token}`,
        'content-type': 'application/json'
      }
    }, response => {
      const chunks = [];
      let completed = false;
      const finish = kind => {
        if (completed) return;
        completed = true;
        resolve({ kind, body: Buffer.concat(chunks).toString('utf8') });
      };
      response.on('data', chunk => chunks.push(chunk));
      response.on('end', () => finish('end'));
      response.on('aborted', () => finish('aborted'));
      response.on('error', () => finish('error'));
    });
    req.on('error', () => resolve({ kind: 'request_error', body: '' }));
    req.end(JSON.stringify({ model: 'gpt-test', stream: true }));
  });
}

async function main() {
  const upstream = http.createServer((req, response) => {
    if (req.url === '/v1/responses') {
      response.writeHead(200, {
        'content-type': 'text/event-stream',
        'x-request-id': 'upstream-normal-id'
      });
      response.write('data: {"type":"response.output_text.delta","delta":"ok"}\n\n');
      response.end('data: [DONE]\n\n');
      return;
    }
    response.writeHead(200, {
      'content-type': 'text/event-stream',
      'content-length': '1000',
      'content-encoding': 'identity',
      'x-request-id': 'upstream-aborted-id'
    });
    response.flushHeaders();
    response.write('data: {"type":"response.output_text.delta","delta":"partial"}\n\n');
    setTimeout(() => response.socket.destroy(), 30);
  });
  const upstreamPort = await listen(upstream);
  const reserved = http.createServer();
  const gatewayPort = await listen(reserved);
  await new Promise(resolve => reserved.close(resolve));

  const gateway = spawn(process.execPath,
    [path.resolve(__dirname, '../assets/local_gateway.js')], {
      env: {
        ...process.env,
        AEGISY_GATEWAY_PORT: String(gatewayPort),
        AEGISY_GATEWAY_TOKEN: token
      },
      stdio: ['pipe', 'pipe', 'pipe']
    });
  const events = [];
  let stdout = '';
  gateway.stdout.on('data', chunk => {
    stdout += chunk.toString('utf8');
    let newline = -1;
    while ((newline = stdout.indexOf('\n')) >= 0) {
      const line = stdout.slice(0, newline).trim();
      stdout = stdout.slice(newline + 1);
      if (line) events.push(JSON.parse(line));
    }
  });
  const waitFor = async predicate => {
    const deadline = Date.now() + 5000;
    while (!predicate() && Date.now() < deadline) {
      await new Promise(resolve => setTimeout(resolve, 10));
    }
    assert(predicate(), 'timed out waiting for gateway event');
  };
  await waitFor(() => events.some(event => event.type === 'ready'));
  gateway.stdin.write(`${JSON.stringify({
    type: 'configure',
    tool: 'codex',
    apiKey: upstreamKey,
    upstream: `http://127.0.0.1:${upstreamPort}`
  })}\n`);
  await waitFor(() => events.some(event => event.type === 'configured'));

  const normal = await request(gatewayPort, '/v1/responses');
  assert.strictEqual(normal.kind, 'end');
  assert(normal.body.includes('[DONE]'));
  await waitFor(() => events.some(event => event.type === 'request'
    && event.upstream_request_id === 'upstream-normal-id'));
  const normalEvent = events.find(event => event.type === 'request'
    && event.upstream_request_id === 'upstream-normal-id');
  assert.strictEqual(normalEvent.termination, 'end');
  assert(normalEvent.bytes_received > 0);
  assert(normalEvent.request_id);

  await request(gatewayPort, '/v1/abort');
  await waitFor(() => events.some(event => event.type === 'request'
    && event.termination !== 'end'));
  const abortedEvent = events.find(event => event.type === 'request'
    && event.termination !== 'end');
  assert(['aborted', 'error'].includes(abortedEvent.termination));
  assert(!JSON.stringify(events).includes(upstreamKey));

  gateway.stdin.write('{"type":"shutdown"}\n');
  await new Promise(resolve => gateway.once('exit', resolve));
  await new Promise(resolve => upstream.close(resolve));
}

main().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
