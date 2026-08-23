'use strict';

const http = require('http');
const https = require('https');
const crypto = require('crypto');
const readline = require('readline');

const port = Number(process.env.AEGISY_GATEWAY_PORT || '43112');
const localToken = process.env.AEGISY_GATEWAY_TOKEN || '';
const profiles = new Map();
const pendingProfiles = new Map();
const profileRevisions = new Map();

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

function safeHeader(value) {
  return String(value || '').replace(/[\r\n]/g, '').slice(0, 160);
}

function responseHeaders(headers) {
  const result = { ...headers };
  for (const name of [
    'connection', 'keep-alive', 'proxy-authenticate', 'proxy-authorization',
    'te', 'trailer', 'transfer-encoding', 'upgrade',
    'x-aegisy-error-schema', 'x-aegisy-error-kind', 'x-aegisy-error-class',
    'x-aegisy-error-retryable', 'x-aegisy-upstream-status'
  ]) {
    delete result[name];
  }
  return result;
}

function routeRequest(url, headers) {
  const match = url.match(/^\/tools\/(claude|codex|gemini|opencode)(\/.*|$)/);
  if (match) {
    return { tool: match[1], path: match[2] || '/' };
  }
  return { tool: String(headers['x-aegisy-tool'] || 'codex'), path: url };
}

function routeModel(tool, routePath) {
  if (tool !== 'gemini') return '';
  const match = String(routePath).match(/\/models\/([^/:?]+)(?::[^?]+)?/);
  if (!match) return '';
  try { return decodeURIComponent(match[1]); } catch (_) { return match[1]; }
}

function finiteNumber(value) {
  const number = Number(value);
  return Number.isFinite(number) && number >= 0 ? Math.trunc(number) : null;
}

// Keep this vocabulary aligned with the AAP provider-error/0.1 projection.
// Classification is based only on transport state and HTTP metadata; response
// bodies are intentionally never parsed or copied into gateway events.
function classifyUpstreamFailure(status, termination) {
  const httpStatus = Number.isInteger(status) && status >= 0 && status <= 65535
    ? status : null;
  if (termination === 'aborted') {
    return {
      schema_version: 'provider-error/0.1', source: 'aegisy-gateway',
      kind: 'response-stream-disconnected', class: 'transport',
      http_status: httpStatus, retryable: true,
      response_body_included: false, credentials_included: false
    };
  }
  if (termination === 'error' || termination === 'request_error') {
    return {
      schema_version: 'provider-error/0.1', source: 'aegisy-gateway',
      kind: 'http-connection-failed', class: 'transport',
      http_status: httpStatus, retryable: true,
      response_body_included: false, credentials_included: false
    };
  }
  if (termination === 'client_closed') return null;
  if (httpStatus === null || httpStatus < 400) return null;
  let kind = 'http-error';
  let retryable = false;
  let errorClass = 'provider';
  if (httpStatus === 408) {
    kind = 'request-timeout';
    errorClass = 'timeout';
    retryable = true;
  } else if (httpStatus === 401 || httpStatus === 403) {
    kind = 'unauthorized';
  } else if ([400, 404, 405, 406, 409, 413, 415, 422].includes(httpStatus)) {
    kind = 'bad-request';
  } else if (httpStatus === 429) {
    kind = 'rate-limit';
    retryable = true;
  } else if ([500, 502, 503, 504].includes(httpStatus)) {
    kind = 'server-overloaded';
    retryable = true;
  }
  return {
    schema_version: 'provider-error/0.1', source: 'aegisy-gateway',
    kind, class: errorClass, http_status: httpStatus, retryable,
    response_body_included: false, credentials_included: false
  };
}

function providerErrorHeaders(error) {
  if (!error) return {};
  const headers = {
    'x-aegisy-error-schema': error.schema_version,
    'x-aegisy-error-kind': error.kind,
    'x-aegisy-error-class': error.class,
    'x-aegisy-error-retryable': error.retryable ? 'true' : 'false'
  };
  if (error.http_status !== null) headers['x-aegisy-upstream-status'] = String(error.http_status);
  return headers;
}

function mergeUsage(target, value) {
  if (!value || typeof value !== 'object') return;
  const input = finiteNumber(value.input_tokens ?? value.prompt_tokens
    ?? value.promptTokenCount ?? value.inputTokens);
  const output = finiteNumber(value.output_tokens ?? value.completion_tokens
    ?? value.candidatesTokenCount ?? value.outputTokens);
  const total = finiteNumber(value.total_tokens ?? value.totalTokenCount
    ?? value.totalTokens);
  if (input !== null) target.inputTokens = Math.max(target.inputTokens ?? 0, input);
  if (output !== null) target.outputTokens = Math.max(target.outputTokens ?? 0, output);
  if (total !== null) target.totalTokens = Math.max(target.totalTokens ?? 0, total);
}

function inspectUsagePayload(value, target) {
  if (!value || typeof value !== 'object') return;
  mergeUsage(target, value.usage);
  mergeUsage(target, value.usageMetadata);
  mergeUsage(target, value.message && value.message.usage);
  mergeUsage(target, value.response && value.response.usage);
  if (value.type === 'message_start') mergeUsage(target, value.message && value.message.usage);
  if (value.type === 'message_delta') mergeUsage(target, value.usage);
}

function requestMetadata(value) {
  if (!value || typeof value !== 'object') {
    return { model: '', reasoningEffort: '', contextLimit: null };
  }
  let reasoningEffort = typeof value.reasoning_effort === 'string'
    ? value.reasoning_effort : '';
  if (!reasoningEffort && value.reasoning && typeof value.reasoning.effort === 'string') {
    reasoningEffort = value.reasoning.effort;
  }
  if (!reasoningEffort && value.thinking && typeof value.thinking === 'object') {
    const budget = finiteNumber(value.thinking.budget_tokens);
    reasoningEffort = budget !== null ? `budget ${budget}` : String(value.thinking.type || '');
  }
  const thinkingConfig = value.generationConfig && value.generationConfig.thinkingConfig;
  if (!reasoningEffort && thinkingConfig && typeof thinkingConfig === 'object') {
    const budget = finiteNumber(thinkingConfig.thinkingBudget);
    reasoningEffort = budget !== null ? `budget ${budget}` : '';
  }
  return {
    model: typeof value.model === 'string' ? value.model : '',
    reasoningEffort,
    contextLimit: finiteNumber(value.context_window ?? value.model_context_window)
  };
}

function appendUsageFields(event, usage) {
  if (usage.inputTokens !== null) event.input_tokens = usage.inputTokens;
  if (usage.outputTokens !== null) event.output_tokens = usage.outputTokens;
  if (usage.totalTokens !== null) event.total_tokens = usage.totalTokens;
  else if (usage.inputTokens !== null && usage.outputTokens !== null) {
    event.total_tokens = usage.inputTokens + usage.outputTokens;
  }
  return event;
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
    let metadata = { model: '', reasoningEffort: '', contextLimit: null };
    if (body.length && String(request.headers['content-type'] || '').includes('application/json')) {
      try {
        const parsed = JSON.parse(body.toString('utf8'));
        metadata = requestMetadata(parsed);
      } catch (_) {
      }
    }
    if (!metadata.model) metadata.model = routeModel(route.tool, route.path);

    const startedEvent = {
      type: 'request_started',
      request_id: typeof crypto.randomUUID === 'function'
        ? crypto.randomUUID() : crypto.randomBytes(16).toString('hex'),
      timestamp: new Date().toISOString(),
      tool: route.tool,
      method: request.method || 'GET',
      path: route.path.split('?')[0],
      model: metadata.model,
      reasoning_effort: metadata.reasoningEffort
    };
    if (metadata.contextLimit !== null) startedEvent.context_limit = metadata.contextLimit;
    emit(startedEvent);

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
    const requestId = startedEvent.request_id;
    // Flush small SSE chunks immediately instead of coalescing them.
    if (request.socket && typeof request.socket.setNoDelay === 'function') {
      request.socket.setNoDelay(true);
    }
    const transport = upstream.protocol === 'http:' ? http : https;
    let terminalEventEmitted = false;
    let clientClosed = false;
    let upstreamTerminating = false;
    let upstreamRequest = null;
    response.on('close', () => {
      if (response.writableEnded || upstreamTerminating) return;
      clientClosed = true;
      if (upstreamRequest && !upstreamRequest.destroyed) {
        upstreamRequest.destroy(new Error('Client connection closed'));
      }
    });
    upstreamRequest = transport.request({
      protocol: upstream.protocol,
      hostname: upstream.hostname,
      port: upstream.port || (upstream.protocol === 'http:' ? 80 : 443),
      method: request.method,
      path: upstreamPath.pathname + upstreamPath.search,
      headers,
      timeout: 20 * 60 * 1000
    }, upstreamResponse => {
      if (upstreamRequest.socket && typeof upstreamRequest.socket.setNoDelay === 'function') {
        upstreamRequest.socket.setNoDelay(true);
      }
      const upstreamStatus = upstreamResponse.statusCode || 502;
      const responseError = classifyUpstreamFailure(upstreamStatus, 'end');
      response.writeHead(upstreamStatus, {
        ...responseHeaders(upstreamResponse.headers),
        ...providerErrorHeaders(responseError)
      });
      const usage = { inputTokens: null, outputTokens: null, totalTokens: null };
      const contentType = String(upstreamResponse.headers['content-type'] || '').toLowerCase();
      const contentEncoding = safeHeader(upstreamResponse.headers['content-encoding']);
      const transferEncoding = safeHeader(upstreamResponse.headers['transfer-encoding']);
      const upstreamRequestId = safeHeader(
        upstreamResponse.headers['x-request-id'] || upstreamResponse.headers['request-id']);
      const isEventStream = contentType.includes('text/event-stream');
      let eventBuffer = '';
      let jsonBuffer = Buffer.alloc(0);
      let bytesReceived = 0;
      const emitTerminalEvent = (status, termination) => {
        if (terminalEventEmitted) return;
        terminalEventEmitted = true;
        const providerError = classifyUpstreamFailure(status, termination);
        const event = appendUsageFields({
          type: 'request',
          request_id: requestId,
          timestamp: new Date().toISOString(),
          tool: route.tool,
          method: request.method || 'GET',
          path: route.path.split('?')[0],
          model: metadata.model,
          reasoning_effort: metadata.reasoningEffort,
          status,
          latency_ms: Date.now() - startedAt,
          termination,
          bytes_received: bytesReceived,
          content_type: safeHeader(contentType),
          content_encoding: contentEncoding,
          transfer_encoding: transferEncoding,
          upstream_request_id: upstreamRequestId
        }, usage);
        if (providerError) event.provider_error = providerError;
        emit(event);
      };
      const inspectEventLine = lineValue => {
        const line = lineValue.trim();
        if (!line.startsWith('data:')) return;
        const data = line.slice(5).trim();
        if (!data || data === '[DONE]') return;
        try { inspectUsagePayload(JSON.parse(data), usage); } catch (_) {}
      };
      upstreamResponse.on('data', chunk => {
        bytesReceived += chunk.length;
        if (!response.write(chunk)) {
          upstreamResponse.pause();
          response.once('drain', () => upstreamResponse.resume());
        }
        if (isEventStream) {
          eventBuffer += chunk.toString('utf8');
          if (eventBuffer.length > 1024 * 1024) eventBuffer = eventBuffer.slice(-1024 * 1024);
          let newline = -1;
          while ((newline = eventBuffer.indexOf('\n')) >= 0) {
            const line = eventBuffer.slice(0, newline);
            eventBuffer = eventBuffer.slice(newline + 1);
            inspectEventLine(line);
          }
        } else if (jsonBuffer.length < 4 * 1024 * 1024) {
          const remaining = 4 * 1024 * 1024 - jsonBuffer.length;
          jsonBuffer = Buffer.concat([jsonBuffer, chunk.subarray(0, remaining)]);
        }
      });
      upstreamResponse.on('end', () => {
        response.end();
        if (isEventStream && eventBuffer.trim()) {
          inspectEventLine(eventBuffer);
        } else if (!isEventStream && jsonBuffer.length) {
          try { inspectUsagePayload(JSON.parse(jsonBuffer.toString('utf8')), usage); } catch (_) {}
        }
        emitTerminalEvent(upstreamResponse.statusCode || 0, 'end');
      });
      upstreamResponse.on('aborted', () => {
        upstreamTerminating = true;
        if (!response.destroyed) response.destroy();
        emitTerminalEvent(upstreamStatus, 'aborted');
      });
      upstreamResponse.on('error', () => {
        upstreamTerminating = true;
        if (!response.destroyed) response.destroy();
        emitTerminalEvent(upstreamStatus, 'error');
      });
    });

    upstreamRequest.on('timeout', () => upstreamRequest.destroy(new Error('Upstream timeout')));
    upstreamRequest.on('error', () => {
      if (terminalEventEmitted) return;
      terminalEventEmitted = true;
      if (!response.headersSent) {
        sendJson(response, 502, { error: { message: 'Aegisy upstream request failed' } });
      } else {
        response.destroy();
      }
      const providerError = classifyUpstreamFailure(null,
        clientClosed ? 'client_closed' : 'request_error');
      const event = {
        type: 'request',
        request_id: requestId,
        timestamp: new Date().toISOString(),
        tool: route.tool,
        method: request.method || 'GET',
        path: route.path.split('?')[0],
        model: metadata.model,
        reasoning_effort: metadata.reasoningEffort,
        status: 502,
        latency_ms: Date.now() - startedAt,
        termination: clientClosed ? 'client_closed' : 'request_error',
        bytes_received: 0
      };
      if (providerError) event.provider_error = providerError;
      emit(event);
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
    if (message.schema === 'aegisy-gateway-control/0.1'
        && message.type === 'control') {
      const requestId = String(message.request_id || '');
      const transactionId = String(message.transaction_id || '');
      const operation = String(message.operation || '');
      const tool = String(message.tool || '');
      const validId = value => /^[A-Za-z0-9_-]{1,128}$/.test(value);
      const supportedTool = ['claude', 'codex', 'gemini', 'opencode'].includes(tool);
      const currentRevision = Number(profileRevisions.get(tool) || 0);
      let outcome = 'rejected';
      let errorCode = 'gateway-control-invalid';
      let revision = currentRevision;
      const commonKeys = [
        'expected_revision', 'operation', 'request_id', 'schema',
        'tool', 'transaction_id', 'type'
      ];
      const expectedKeys = operation === 'prepare-configure'
        ? [...commonKeys, 'apiKey', 'upstream'].sort()
        : commonKeys.sort();
      const exactFields = Object.keys(message).sort().join('\0')
        === expectedKeys.join('\0');
      if (validId(requestId) && validId(transactionId) && supportedTool
          && exactFields
          && Number.isSafeInteger(message.expected_revision)
          && message.expected_revision >= 0) {
        if (operation === 'prepare-configure' && typeof message.apiKey === 'string'
            && message.apiKey.trim() && message.expected_revision === currentRevision
            && !pendingProfiles.has(tool)) {
          pendingProfiles.set(tool, {
            transactionId,
            revision: currentRevision,
            apiKey: message.apiKey,
            upstream: String(message.upstream || 'https://aegisy.cc')
          });
          outcome = 'prepared';
          errorCode = '';
        } else if (operation === 'prepare-remove'
            && message.expected_revision === currentRevision
            && !pendingProfiles.has(tool)) {
          pendingProfiles.set(tool, {
            transactionId, revision: currentRevision, remove: true
          });
          outcome = 'prepared';
          errorCode = '';
        } else if (operation === 'commit') {
          const pending = pendingProfiles.get(tool);
          if (pending && pending.transactionId === transactionId
              && pending.revision === currentRevision
              && message.expected_revision === currentRevision) {
            if (pending.remove) profiles.delete(tool);
            else profiles.set(tool, {
              apiKey: pending.apiKey,
              upstream: pending.upstream
            });
            profileRevisions.set(tool, currentRevision + 1);
            pendingProfiles.delete(tool);
            revision = currentRevision + 1;
            outcome = 'committed';
            errorCode = '';
          }
        } else if (operation === 'abort') {
          const pending = pendingProfiles.get(tool);
          if (pending && pending.transactionId === transactionId
              && pending.revision === currentRevision
              && message.expected_revision === currentRevision) {
            pendingProfiles.delete(tool);
            outcome = 'aborted';
            errorCode = '';
          }
        }
      }
      emit({
        schema: 'aegisy-gateway-control/0.1', type: 'control-result',
        request_id: requestId, transaction_id: transactionId,
        operation, tool, outcome, revision,
        credential_included: false, error_code: errorCode
      });
    } else if (message.type === 'shutdown') {
      server.close(() => process.exit(0));
    }
  } catch (_) {
    emit({ type: 'warning', message: 'Ignored invalid control message' });
  }
});
