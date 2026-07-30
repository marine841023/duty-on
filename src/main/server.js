/**
 * HTTP Server - Receives hook events from Trae IDE and serves status API.
 *
 * Endpoints:
 *   POST /hook          - Receive a hook event from the bridge script
 *   GET  /status        - Get current state snapshot (for debugging)
 *   GET  /health        - Health check
 *   POST /unregister    - Remove a session (when IDE closes)
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

    if (req.method === 'OPTIONS') {
      res.writeHead(204);
      res.end();
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
          res.writeHead(200, { 'Content-Type': 'application/json' });
          res.end(JSON.stringify({ ok: true }));
        } catch (err) {
          res.writeHead(400, { 'Content-Type': 'application/json' });
          res.end(JSON.stringify({ error: err.message }));
        }
      });
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
          res.writeHead(200, { 'Content-Type': 'application/json' });
          res.end(JSON.stringify({ ok: true }));
        } catch (err) {
          res.writeHead(400, { 'Content-Type': 'application/json' });
          res.end(JSON.stringify({ error: err.message }));
        }
      });
      return;
    }

    // GET /status - Current state snapshot
    if (req.method === 'GET' && req.url === '/status') {
      const snapshot = stateManager.getSnapshot();
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify(snapshot, null, 2));
      return;
    }

    // GET /health - Health check
    if (req.method === 'GET' && req.url === '/health') {
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ status: 'ok', port: PORT }));
      return;
    }

    // 404
    res.writeHead(404, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ error: 'Not found' }));
  });

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
