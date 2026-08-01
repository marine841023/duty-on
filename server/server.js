/**
 * TraePet Update Server
 *
 * A zero-dependency Node.js HTTP server that serves version manifests and
 * installer files for electron-updater's "generic" provider.
 *
 * Endpoints:
 *   GET  /updates/latest.yml          — version manifest (electron-updater checks this)
 *   GET  /updates/<filename>          — download installer / blockmap / yaml
 *   POST /publish                     — upload a new version (token-protected)
 *   GET  /health                      — health check
 *   GET  /                            — simple status page
 *
 * Usage:
 *   node server.js                    — start on port 17522
 *   PORT=8080 node server.js          — custom port
 *   UPLOAD_TOKEN=secret node server.js — set upload auth token
 *
 * Files are stored in ./uploads/ (created automatically).
 * The publish script (scripts/publish.js) uploads build artifacts here.
 */

const http = require('http');
const fs = require('fs');
const path = require('path');

const PORT = process.env.PORT || 17522;
const HOST = process.env.HOST || '0.0.0.0';
const UPLOAD_TOKEN = process.env.UPLOAD_TOKEN || 'trae-pet-upload-token';
const UPLOAD_DIR = path.join(__dirname, 'uploads');

// Ensure uploads directory exists
if (!fs.existsSync(UPLOAD_DIR)) {
  fs.mkdirSync(UPLOAD_DIR, { recursive: true });
}

const MIME_TYPES = {
  '.yml': 'text/yaml; charset=utf-8',
  '.yaml': 'text/yaml; charset=utf-8',
  '.exe': 'application/octet-stream',
  '.blockmap': 'application/octet-stream',
  '.json': 'application/json',
};

const server = http.createServer((req, res) => {
  // CORS — allow electron-updater (local) and publish script (any origin)
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type, Authorization, X-Filename');

  if (req.method === 'OPTIONS') {
    res.writeHead(204);
    res.end();
    return;
  }

  // GET /health
  if (req.method === 'GET' && req.url === '/health') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ status: 'ok', port: PORT }));
    return;
  }

  // GET / — status page
  if (req.method === 'GET' && (req.url === '/' || req.url === '')) {
    const files = listUploadFiles();
    const latestVersion = readLatestVersion();
    const html = `<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>TraePet Update Server</title>
<style>body{font-family:sans-serif;max-width:600px;margin:40px auto;padding:0 20px}
h1{color:#333}code{background:#f4f4f4;padding:2px 6px;border-radius:3px}
.file{padding:4px 0;border-bottom:1px solid #eee}</style>
</head><body>
<h1>TraePet Update Server</h1>
<p>Latest version: <strong>${latestVersion || 'none'}</strong></p>
<p>Port: ${PORT}</p>
<h2>Available files (${files.length})</h2>
${files.map((f) => `<div class="file">${f.name} — ${(f.size / 1048576).toFixed(2)} MB</div>`).join('') || '<p>No files uploaded yet.</p>'}
</body></html>`;
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
    res.end(html);
    return;
  }

  // GET /updates/<filename> — serve static files (latest.yml, installer, blockmap)
  if (req.method === 'GET' && req.url.startsWith('/updates/')) {
    const filename = path.basename(req.url);
    // Prevent path traversal
    if (filename.includes('..') || filename.includes('/') || filename.includes('\\')) {
      res.writeHead(400);
      res.end('Bad request');
      return;
    }
    const filePath = path.join(UPLOAD_DIR, filename);

    if (!fs.existsSync(filePath)) {
      res.writeHead(404, { 'Content-Type': 'text/plain' });
      res.end(`Not found: ${filename}`);
      return;
    }

    const ext = path.extname(filename).toLowerCase();
    const mime = MIME_TYPES[ext] || 'application/octet-stream';
    res.setHeader('Content-Type', mime);

    // Support range requests for large installer downloads
    const stat = fs.statSync(filePath);
    res.setHeader('Accept-Ranges', 'bytes');
    res.setHeader('Content-Length', stat.size);

    const range = req.headers.range;
    if (range) {
      const parts = range.replace(/bytes=/, '').split('-');
      const start = parseInt(parts[0], 10) || 0;
      const end = parts[1] ? parseInt(parts[1], 10) : stat.size - 1;
      res.writeHead(206, {
        'Content-Range': `bytes ${start}-${end}/${stat.size}`,
        'Content-Length': end - start + 1,
      });
      fs.createReadStream(filePath, { start, end }).pipe(res);
    } else {
      res.writeHead(200);
      fs.createReadStream(filePath).pipe(res);
    }
    return;
  }

  // POST /publish — upload a new version (token-protected)
  // Headers: Authorization: Bearer <token>, X-Filename: <filename>
  // Body: raw file content
  if (req.method === 'POST' && req.url === '/publish') {
    const auth = req.headers.authorization;
    if (auth !== `Bearer ${UPLOAD_TOKEN}`) {
      res.writeHead(401, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ error: 'Unauthorized' }));
      return;
    }

    const filename = req.headers['x-filename'];
    if (!filename || filename.includes('..') || filename.includes('/') || filename.includes('\\')) {
      res.writeHead(400, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ error: 'Invalid or missing X-Filename header' }));
      return;
    }

    const filePath = path.join(UPLOAD_DIR, filename);
    const writeStream = fs.createWriteStream(filePath);

    req.pipe(writeStream);

    writeStream.on('finish', () => {
      const stat = fs.statSync(filePath);
      console.log(`[Server] Uploaded: ${filename} (${(stat.size / 1048576).toFixed(2)} MB)`);
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ ok: true, filename, size: stat.size }));
    });

    writeStream.on('error', (err) => {
      console.error('[Server] Upload error:', err.message);
      res.writeHead(500, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ error: err.message }));
    });
    return;
  }

  // 404
  res.writeHead(404, { 'Content-Type': 'text/plain' });
  res.end('Not found');
});

server.listen(PORT, HOST, () => {
  console.log(`[TraePet Update Server] Listening on http://${HOST}:${PORT}`);
  console.log(`[TraePet Update Server] Upload token: ${UPLOAD_TOKEN}`);
  console.log(`[TraePet Update Server] Uploads directory: ${UPLOAD_DIR}`);
  const files = listUploadFiles();
  if (files.length > 0) {
    console.log(`[TraePet Update Server] Available files:`);
    for (const f of files) {
      console.log(`  ${f.name} (${(f.size / 1048576).toFixed(2)} MB)`);
    }
  }
});

// ===== Helpers =====

function listUploadFiles() {
  try {
    return fs.readdirSync(UPLOAD_DIR)
      .filter((name) => !name.startsWith('.'))
      .map((name) => {
        const stat = fs.statSync(path.join(UPLOAD_DIR, name));
        return { name, size: stat.size, mtime: stat.mtime };
      })
      .sort((a, b) => b.mtime - a.mtime);
  } catch {
    return [];
  }
}

function readLatestVersion() {
  try {
    const ymlPath = path.join(UPLOAD_DIR, 'latest.yml');
    if (!fs.existsSync(ymlPath)) return null;
    const content = fs.readFileSync(ymlPath, 'utf-8');
    const match = content.match(/^version:\s*(.+)$/m);
    return match ? match[1].trim() : null;
  } catch {
    return null;
  }
}
