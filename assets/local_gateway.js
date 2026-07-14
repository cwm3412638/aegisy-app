'use strict';

const http = require('http');
const https = require('https');
const readline = require('readline');

const port = Number(process.env.AEGISY_GATEWAY_PORT || '43112');
const localToken = process.env.AEGISY_GATEWAY_TOKEN || '';
const profiles = new Map();

function emit(event) {
  process.stdout.write(`${JSON.stringify(event)}\n`);
}

function sendJson(response, status, data) {
  const body = Buffer.from(JSON.stringify(data));
  response.writeHead(status, {
    'content-type': 'application/json; charset=utf-8',
    'content-length': String(body.length)
  });
  response.end(body);
}

function authorized(request) {
  const requestUrl = new URL(request.url || '/', `http://127.0.0.1:${port}`);
  return request.headers.authorization === `Bearer ${localToken}`
    || request.headers['x-api-key'] === localToken
    || request.headers['x-goog-api-key'] === localToken
    || requestUrl.searchParams.get('key') === localToken;
}

function routeRequest(url, headers) {
  const match = url.match(/^\/tools\/(claude|codex|gemini|opencode)(\/.*|$)/);
  if (match) {
    return { tool: match[1], path: match[2] || '/' };
  }
  return { tool: String(headers['x-aegisy-tool'] || 'codex'), path: url };
}

const server = http.createServer((request, response) => {
  if (request.url === '/health') {
    sendJson(response, 200, {
      status: 'ok',
      configured_tools: Array.from(profiles.keys()),
      port
    });
    return;
  }
  if (!authorized(request)) {
    sendJson(response, 401, { error: { message: 'Invalid local gateway token' } });
    return;
  }

  const route = routeRequest(request.url || '/', request.headers);
  const profile = profiles.get(route.tool);
  if (!profile || !profile.apiKey) {
    sendJson(response, 503, { error: { message: `No active ${route.tool} profile` } });
    return;
  }

  const chunks = [];
  let size = 0;
  request.on('data', chunk => {
    size += chunk.length;
    if (size <= 32 * 1024 * 1024) chunks.push(chunk);
  });
  request.on('end', () => {
    if (size > 32 * 1024 * 1024) {
      sendJson(response, 413, { error: { message: 'Request body is too large' } });
      return;
    }

    const body = Buffer.concat(chunks);
    let model = '';
    if (body.length && String(request.headers['content-type'] || '').includes('application/json')) {
      try {
        const parsed = JSON.parse(body.toString('utf8'));
        model = typeof parsed.model === 'string' ? parsed.model : '';
      } catch (_) {
      }
    }

    const upstream = new URL(profile.upstream || 'https://aegisy.cc');
    const upstreamPath = new URL(route.path, `http://127.0.0.1:${port}`);
    upstreamPath.searchParams.delete('key');
    const headers = { ...request.headers };
    delete headers.host;
    delete headers.authorization;
    delete headers['x-api-key'];
    delete headers['x-goog-api-key'];
    delete headers['content-length'];
    // Force an uncompressed upstream response. gzip/br buffer SSE events
    // internally and only flush once a block fills, which starves Codex's
    // SSE reader and triggers "idle timeout waiting for SSE".
    delete headers['accept-encoding'];
    headers['accept-encoding'] = 'identity';
    headers.authorization = `Bearer ${profile.apiKey}`;
    headers['x-api-key'] = profile.apiKey;
    headers['x-goog-api-key'] = profile.apiKey;
    if (body.length) headers['content-length'] = String(body.length);

    const startedAt = Date.now();
    // Flush small SSE chunks immediately instead of coalescing them.
    if (request.socket && typeof request.socket.setNoDelay === 'function') {
      request.socket.setNoDelay(true);
    }
    const upstreamRequest = https.request({
      protocol: upstream.protocol,
      hostname: upstream.hostname,
      port: upstream.port || 443,
      method: request.method,
      path: upstreamPath.pathname + upstreamPath.search,
      headers,
      timeout: 20 * 60 * 1000
    }, upstreamResponse => {
      if (upstreamRequest.socket && typeof upstreamRequest.socket.setNoDelay === 'function') {
        upstreamRequest.socket.setNoDelay(true);
      }
      response.writeHead(upstreamResponse.statusCode || 502, upstreamResponse.headers);
      upstreamResponse.pipe(response);
      upstreamResponse.on('end', () => {
        emit({
          type: 'request',
          timestamp: new Date().toISOString(),
          tool: route.tool,
          method: request.method || 'GET',
          path: route.path.split('?')[0],
          model,
          status: upstreamResponse.statusCode || 0,
          latency_ms: Date.now() - startedAt
        });
      });
    });

    upstreamRequest.on('timeout', () => upstreamRequest.destroy(new Error('Upstream timeout')));
    upstreamRequest.on('error', error => {
      if (!response.headersSent) {
        sendJson(response, 502, { error: { message: 'Aegisy upstream request failed' } });
      } else {
        response.destroy();
      }
      emit({
        type: 'request',
        timestamp: new Date().toISOString(),
        tool: route.tool,
        method: request.method || 'GET',
        path: route.path.split('?')[0],
        model,
        status: 502,
        latency_ms: Date.now() - startedAt,
        error: String(error.message || error).slice(0, 200)
      });
    });
    if (body.length) upstreamRequest.write(body);
    upstreamRequest.end();
  });
});

server.on('error', error => emit({ type: 'fatal', error: String(error.message || error) }));
server.listen(port, '127.0.0.1', () => emit({ type: 'ready', port }));

readline.createInterface({ input: process.stdin }).on('line', line => {
  try {
    const message = JSON.parse(line);
    if (message.type === 'configure' && message.tool && message.apiKey) {
      profiles.set(String(message.tool), {
        apiKey: String(message.apiKey),
        upstream: String(message.upstream || 'https://aegisy.cc')
      });
      emit({ type: 'configured', tool: String(message.tool) });
    } else if (message.type === 'remove' && message.tool) {
      profiles.delete(String(message.tool));
    } else if (message.type === 'shutdown') {
      server.close(() => process.exit(0));
    }
  } catch (_) {
    emit({ type: 'warning', message: 'Ignored invalid control message' });
  }
});
