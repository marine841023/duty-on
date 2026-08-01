/**
 * HTTP Server - Receives hook events from Trae IDE and serves status API.
 *
 * Endpoints:
 *   POST /hook          - Receive a hook event from the bridge script
 *   GET  /status        - Get current state snapshot (for debugging)
 *   GET  /health        - Health check
 *   POST /unregister    - Remove a session (when IDE closes)
 *
 * Connection handling:
 *   The bridge script sets a 2s timeout and may disconnect before the server
 *   finishes processing. Without explicit cleanup, these half-closed sockets
 *   accumulate as CLOSE_WAIT and eventually exhaust the server's connection
 *   queue — making it unresponsive (root cause of the "pet stuck on idle" bug).
 *   We guard against this by:
 *     1. Tracking an `ended` flag so res.end()/destroy() is called exactly once.
 *     2. Listening for req 'close'/'error' (client disconnect) and destroying res.
 *     3. Setting socket timeouts so idle/stale sockets are reaped automatically.
 */

const http = require('http');
const { PORT, HOST } = require('./config');

function createServer(stateManager) {
  const server = http.createServer((req, res) => {
    // CORS: restrict to loopback only (the bridge script runs locally).
    // A wildcard would allow any web page to POST fake hook events.
    res.setHeader('Access-Control-Allow-Origin', `http://${HOST}:${PORT}`);
    res.setHeader('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
    res.setHeader('Access-Control-Allow-Headers', 'Content-Type');

    // --- Robust response termination (prevents CLOSE_WAIT leaks) ---
    // `ended` guards against double-end (which throws) and ensures that when
    // the client disconnects early we still destroy the socket so it doesn't
    // linger in CLOSE_WAIT.
    let ended = false;
    const safeEnd = (status, contentType, body) => {
      if (ended) return;
      ended = true;
      try {
        if (!res.headersSent) res.writeHead(status, { 'Content-Type': contentType });
        res.end(body);
      } catch (_) {
        try { res.destroy(); } catch (__) { /* socket already gone */ }
      }
    };
    const safeDestroy = () => {
      if (ended) return;
      ended = true;
      try { res.destroy(); } catch (_) { /* already closed */ }
    };

    // Client closed the connection before we finished (e.g. bridge 2s timeout).
    // This is the key fix: without destroying res here, the socket stays in
    // CLOSE_WAIT forever.
    req.on('close', safeDestroy);
    req.on('error', safeDestroy);
    res.on('error', safeDestroy);

    if (req.method === 'OPTIONS') {
      safeEnd(204);
      return;
    }

    // POST /hook - Receive hook event
    if (req.method === 'POST' && req.url === '/hook') {
      let body = '';
      req.on('data', (chunk) => { body += chunk; });
      req.on('end', () => {
        try {
          const event = JSON.parse(body);
          // Diagnostic: log every incoming event so we can confirm Trae IDE
          // hooks are firing. Concise form keeps the log readable.
          console.log(
            `[PetServer] event: ${event.hook_event_name} | session=${event.session_id} | project=${event.project_name || event.cwd || '?'}`
          );
          // Detailed Notification payload for tuning config.NOTIFICATION_*_TYPES.
          if (event.hook_event_name === 'Notification') {
            console.log('[PetServer] Notification payload:', JSON.stringify(event));
          }
          stateManager.handleHookEvent(event);
          safeEnd(200, 'application/json', JSON.stringify({ ok: true }));
        } catch (err) {
          console.error('[PetServer] /hook error:', err.message);
          safeEnd(400, 'application/json', JSON.stringify({ error: err.message }));
        }
      });
      // If the client disconnects mid-body, 'end' won't fire — req 'close'
      // (registered above) handles cleanup.
      return;
    }

    // POST /unregister - Remove a session
    if (req.method === 'POST' && req.url === '/unregister') {
      let body = '';
      req.on('data', (chunk) => { body += chunk; });
      req.on('end', () => {
        try {
          const { session_id } = JSON.parse(body);
          if (session_id) {
            stateManager.removeSession(session_id);
          }
          safeEnd(200, 'application/json', JSON.stringify({ ok: true }));
        } catch (err) {
          safeEnd(400, 'application/json', JSON.stringify({ error: err.message }));
        }
      });
      return;
    }

    // GET /status - Current state snapshot
    if (req.method === 'GET' && req.url === '/status') {
      const snapshot = stateManager.getSnapshot();
      safeEnd(200, 'application/json', JSON.stringify(snapshot, null, 2));
      return;
    }

    // GET /health - Health check
    if (req.method === 'GET' && req.url === '/health') {
      safeEnd(200, 'application/json', JSON.stringify({ status: 'ok', port: PORT }));
      return;
    }

    // 404
    safeEnd(404, 'application/json', JSON.stringify({ error: 'Not found' }));
  });

  // --- Socket timeouts: reap idle/stale connections so CLOSE_WAIT can't pile up ---
  // Default server.timeout is 0 (never), which is why stale sockets accumulated.
  server.timeout = 10000;          // destroy sockets idle > 10s
  server.keepAliveTimeout = 5000;  // close keep-alive after 5s idle
  server.headersTimeout = 8000;    // must receive headers within 8s
  // requestTimeout (Node 18+) caps the whole request including body.
  if ('requestTimeout' in server) server.requestTimeout = 10000;

  server.listen(PORT, HOST, () => {
    console.log(`[PetServer] Listening on http://${HOST}:${PORT}`);
  });

  server.on('error', (err) => {
    if (err.code === 'EADDRINUSE') {
      console.error(`[PetServer] Port ${PORT} is already in use. Another instance may be running.`);
    } else {
      console.error('[PetServer] Error:', err);
    }
  });

  return server;
}

module.exports = { createServer, PORT };
