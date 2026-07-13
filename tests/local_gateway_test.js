'use strict';

const http = require('http');
const path = require('path');
const { spawn } = require('child_process');

const port = 44000 + Math.floor(Math.random() * 1000);
const token = 'gateway-test-token';
const script = path.resolve(__dirname, '../assets/local_gateway.js');
const child = spawn(process.execPath, [script], {
  env: { ...process.env, AEGISY_GATEWAY_PORT: String(port), AEGISY_GATEWAY_TOKEN: token },
  stdio: ['pipe', 'pipe', 'inherit']
});

function request(pathname, authorization) {
  return new Promise((resolve, reject) => {
    const request = http.get({
      hostname: '127.0.0.1', port, path: pathname,
      headers: authorization ? { authorization } : {}
    }, response => {
      const chunks = [];
      response.on('data', chunk => chunks.push(chunk));
      response.on('end', () => resolve({
        status: response.statusCode,
        body: JSON.parse(Buffer.concat(chunks).toString('utf8'))
      }));
    });
    request.on('error', reject);
  });
}

function requestWithHeaders(pathname, headers) {
  return new Promise((resolve, reject) => {
    const request = http.get({ hostname: '127.0.0.1', port, path: pathname, headers }, response => {
      const chunks = [];
      response.on('data', chunk => chunks.push(chunk));
      response.on('end', () => resolve({ status: response.statusCode,
        body: JSON.parse(Buffer.concat(chunks).toString('utf8')) }));
    });
    request.on('error', reject);
  });
}

const timeout = setTimeout(() => {
  child.kill();
  console.error('gateway test timed out');
  process.exit(1);
}, 8000);

let buffer = '';
child.stdout.on('data', async chunk => {
  buffer += chunk.toString('utf8');
  const lines = buffer.split('\n');
  buffer = lines.pop() || '';
  for (const line of lines) {
    if (!line.trim()) continue;
    const event = JSON.parse(line);
    if (event.type !== 'ready') continue;
    try {
      const health = await request('/health');
      const unauthorized = await request('/v1/models');
      const noProfile = await request('/tools/codex/v1/models', `Bearer ${token}`);
      const claudeAuth = await requestWithHeaders('/tools/claude/v1/models', { 'x-api-key': token });
      const geminiAuth = await requestWithHeaders('/tools/gemini/v1beta/models?key=' + token, {});
      if (health.status !== 200 || health.body.status !== 'ok') throw new Error('health check failed');
      if (unauthorized.status !== 401) throw new Error('unauthorized request was accepted');
      if (noProfile.status !== 503) throw new Error('missing profile did not fail safely');
      if (claudeAuth.status !== 503) throw new Error('x-api-key authentication failed');
      if (geminiAuth.status !== 503) throw new Error('Gemini key authentication failed');
      child.stdin.write(`${JSON.stringify({ type: 'shutdown' })}\n`);
    } catch (error) {
      child.kill();
      console.error(error.message);
      process.exit(1);
    }
  }
});

child.on('exit', code => {
  clearTimeout(timeout);
  process.exit(code === 0 ? 0 : 1);
});
