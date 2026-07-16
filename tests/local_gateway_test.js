'use strict';

const http = require('http');
const path = require('path');
const { spawn } = require('child_process');

const port = 44000 + Math.floor(Math.random() * 500);
const upstreamPort = 44500 + Math.floor(Math.random() * 500);
const token = 'gateway-test-token';
const upstreamKey = 'upstream-secret-key';
const secretPrompt = 'private prompt that must never be logged';
const script = path.resolve(__dirname, '../assets/local_gateway.js');

function listen(server, listenPort) {
  return new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(listenPort, '127.0.0.1', resolve);
  });
}

function close(server) {
  return new Promise(resolve => server.close(resolve));
}

function request(method, pathname, headers = {}, body = null) {
  return new Promise((resolve, reject) => {
    const payload = body === null ? null : Buffer.from(JSON.stringify(body));
    const requestHeaders = { ...headers };
    if (payload) {
      requestHeaders['content-type'] = 'application/json';
      requestHeaders['content-length'] = String(payload.length);
    }
    const clientRequest = http.request({
      hostname: '127.0.0.1', port, path: pathname, method,
      headers: requestHeaders
    }, response => {
      const chunks = [];
      response.on('data', chunk => chunks.push(chunk));
      response.on('end', () => {
        const text = Buffer.concat(chunks).toString('utf8');
        let json = null;
        try { json = JSON.parse(text); } catch (_) {}
        resolve({ status: response.statusCode, text, body: json });
      });
    });
    clientRequest.on('error', reject);
    if (payload) clientRequest.write(payload);
    clientRequest.end();
  });
}

function expect(condition, message) {
  if (!condition) throw new Error(message);
}

async function main() {
  const upstream = http.createServer((request, response) => {
    const chunks = [];
    request.on('data', chunk => chunks.push(chunk));
    request.on('end', () => {
      expect(request.headers.authorization === `Bearer ${upstreamKey}`,
        'gateway did not replace the local token with the upstream key');
      if ((request.url || '').includes('/responses')) {
        expect(request.headers['x-openai-actor-authorization'] === 'aegisy',
          'gateway did not forward the Codex capability header');
      }
      if ((request.url || '').includes('/v1/messages')) {
        response.writeHead(200, { 'content-type': 'text/event-stream' });
        response.write('data: {"type":"message_start","message":{"usage":{"input_tokens":640}}}\n\n');
        response.end('data: {"type":"message_delta","usage":{"output_tokens":90}}');
        return;
      }
      if ((request.url || '').includes(':generateContent')) {
        response.writeHead(200, { 'content-type': 'application/json' });
        response.end(JSON.stringify({
          candidates: [{ content: { parts: [{ text: 'private gemini output' }] } }],
          usageMetadata: {
            promptTokenCount: 720,
            candidatesTokenCount: 80,
            totalTokenCount: 800
          }
        }));
        return;
      }
      response.writeHead(200, { 'content-type': 'text/event-stream' });
      response.write('data: {"type":"response.output_text.delta","delta":"secret output"}\n\n');
      response.end('data: {"type":"response.completed","response":{"usage":{"input_tokens":84000,"output_tokens":1200,"total_tokens":85200}}}');
    });
  });
  await listen(upstream, upstreamPort);

  const child = spawn(process.execPath, [script], {
    env: {
      ...process.env,
      AEGISY_GATEWAY_PORT: String(port),
      AEGISY_GATEWAY_TOKEN: token
    },
    stdio: ['pipe', 'pipe', 'inherit']
  });

  const events = [];
  const waiters = [];
  let stdoutBuffer = '';
  child.stdout.on('data', chunk => {
    stdoutBuffer += chunk.toString('utf8');
    const lines = stdoutBuffer.split('\n');
    stdoutBuffer = lines.pop() || '';
    for (const line of lines) {
      if (!line.trim()) continue;
      const event = JSON.parse(line);
      events.push(event);
      for (const waiter of [...waiters]) {
        if (!waiter.predicate(event)) continue;
        waiters.splice(waiters.indexOf(waiter), 1);
        clearTimeout(waiter.timer);
        waiter.resolve(event);
      }
    }
  });

  const waitForEvent = (predicate, description) => new Promise((resolve, reject) => {
    const existing = events.find(predicate);
    if (existing) {
      resolve(existing);
      return;
    }
    const waiter = { predicate, resolve, reject, timer: null };
    waiter.timer = setTimeout(() => {
      const index = waiters.indexOf(waiter);
      if (index >= 0) waiters.splice(index, 1);
      reject(new Error(`timed out waiting for ${description}`));
    }, 5000);
    waiters.push(waiter);
  });

  const childExited = new Promise((resolve, reject) => {
    child.once('exit', code => code === 0 ? resolve() : reject(
      new Error(`gateway exited with code ${code}`)));
  });

  try {
    await waitForEvent(event => event.type === 'ready', 'gateway ready');

    const health = await request('GET', '/health');
    const unauthorized = await request('GET', '/v1/models');
    const noProfile = await request('GET', '/tools/codex/v1/models', {
      authorization: `Bearer ${token}`
    });
    const claudeAuth = await request('GET', '/tools/claude/v1/models', {
      'x-api-key': token
    });
    const geminiAuth = await request('GET', `/tools/gemini/v1beta/models?key=${token}`);
    const openCodeAuth = await request('GET', '/tools/opencode/v1/messages', {
      'x-api-key': token
    });
    expect(health.status === 200 && health.body.status === 'ok', 'health check failed');
    expect(unauthorized.status === 401, 'unauthorized request was accepted');
    expect(noProfile.status === 503, 'missing profile did not fail safely');
    expect(claudeAuth.status === 503, 'x-api-key authentication failed');
    expect(geminiAuth.status === 503, 'Gemini key authentication failed');
    expect(openCodeAuth.status === 503, 'OpenCode route authentication failed');

    const configured = waitForEvent(
      event => event.type === 'configured' && event.tool === 'codex',
      'Codex configuration');
    child.stdin.write(`${JSON.stringify({
      type: 'configure',
      tool: 'codex',
      apiKey: upstreamKey,
      upstream: `http://127.0.0.1:${upstreamPort}`
    })}\n`);
    await configured;

    const started = waitForEvent(event => event.type === 'request_started',
      'request_started event');
    const finished = waitForEvent(event => event.type === 'request',
      'request completion event');
    const proxied = await request('POST', '/tools/codex/v1/responses', {
      authorization: `Bearer ${token}`,
      'x-openai-actor-authorization': 'aegisy'
    }, {
      model: 'gpt-live',
      reasoning: { effort: 'high' },
      context_window: 272000,
      input: secretPrompt
    });
    const startedEvent = await started;
    const finishedEvent = await finished;

    expect(proxied.status === 200 && proxied.text.includes('response.completed'),
      'streaming upstream response was not forwarded');
    expect(startedEvent.tool === 'codex' && startedEvent.model === 'gpt-live',
      'request metadata was not emitted');
    expect(startedEvent.reasoning_effort === 'high'
      && startedEvent.context_limit === 272000,
      'reasoning/context metadata was not emitted');
    expect(finishedEvent.input_tokens === 84000
      && finishedEvent.output_tokens === 1200
      && finishedEvent.total_tokens === 85200,
      'usage metadata was not parsed from the SSE completion');

    const serializedEvents = JSON.stringify(events);
    expect(!serializedEvents.includes(secretPrompt), 'prompt content leaked into gateway events');
    expect(!serializedEvents.includes('secret output'), 'response content leaked into gateway events');
    expect(!serializedEvents.includes(upstreamKey), 'upstream credential leaked into gateway events');

    for (const tool of ['claude', 'gemini']) {
      const toolConfigured = waitForEvent(
        event => event.type === 'configured' && event.tool === tool,
        `${tool} configuration`);
      child.stdin.write(`${JSON.stringify({
        type: 'configure', tool, apiKey: upstreamKey,
        upstream: `http://127.0.0.1:${upstreamPort}`
      })}\n`);
      await toolConfigured;
    }

    const claudeStarted = waitForEvent(
      event => event.type === 'request_started' && event.tool === 'claude',
      'Claude request start');
    const claudeFinished = waitForEvent(
      event => event.type === 'request' && event.tool === 'claude',
      'Claude request completion');
    await request('POST', '/tools/claude/v1/messages', {
      'x-api-key': token
    }, {
      model: 'claude-live',
      thinking: { type: 'enabled', budget_tokens: 16000 },
      messages: [{ role: 'user', content: secretPrompt }]
    });
    const claudeStartedEvent = await claudeStarted;
    const claudeFinishedEvent = await claudeFinished;
    expect(claudeStartedEvent.reasoning_effort === 'budget 16000',
      'Anthropic thinking metadata was not parsed');
    expect(claudeFinishedEvent.input_tokens === 640
      && claudeFinishedEvent.output_tokens === 90
      && claudeFinishedEvent.total_tokens === 730,
      'Anthropic streaming usage was not parsed');

    const geminiStarted = waitForEvent(
      event => event.type === 'request_started' && event.tool === 'gemini',
      'Gemini request start');
    const geminiFinished = waitForEvent(
      event => event.type === 'request' && event.tool === 'gemini',
      'Gemini request completion');
    await request('POST', '/tools/gemini/v1beta/models/gemini-live:generateContent', {
      'x-goog-api-key': token
    }, {
      generationConfig: { thinkingConfig: { thinkingBudget: 8000 } },
      contents: [{ parts: [{ text: secretPrompt }] }]
    });
    const geminiStartedEvent = await geminiStarted;
    const geminiFinishedEvent = await geminiFinished;
    expect(geminiStartedEvent.model === 'gemini-live',
      'Gemini route model was not parsed');
    expect(geminiStartedEvent.reasoning_effort === 'budget 8000',
      'Gemini thinking metadata was not parsed');
    expect(geminiFinishedEvent.input_tokens === 720
      && geminiFinishedEvent.output_tokens === 80
      && geminiFinishedEvent.total_tokens === 800,
      'Gemini JSON usageMetadata was not parsed');

    const allEvents = JSON.stringify(events);
    expect(!allEvents.includes(secretPrompt), 'provider prompt content leaked into gateway events');
    expect(!allEvents.includes('private gemini output'),
      'provider response content leaked into gateway events');

    child.stdin.write(`${JSON.stringify({ type: 'shutdown' })}\n`);
    await childExited;
  } finally {
    if (!child.killed && child.exitCode === null) child.kill();
    await close(upstream);
  }
}

const timeout = setTimeout(() => {
  console.error('gateway test timed out');
  process.exit(1);
}, 12000);

main().then(() => {
  clearTimeout(timeout);
}).catch(error => {
  clearTimeout(timeout);
  console.error(error.message);
  process.exit(1);
});
