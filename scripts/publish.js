#!/usr/bin/env node
/**
 * Publish script — uploads build artifacts (latest.yml + installer) to the
 * update server.
 *
 * Usage:
 *   node scripts/publish.js                          — upload to default server
 *   SERVER_URL=http://example.com:17522 node scripts/publish.js
 *   UPLOAD_TOKEN=secret node scripts/publish.js
 *
 * Prerequisites:
 *   1. Run `npm run dist` first — generates release/TraePet-Setup-x.y.z.exe
 *      and release/latest.yml
 *   2. Start the update server: `node server/server.js`
 *   3. Run this script to upload.
 */

const fs = require('fs');
const path = require('path');
const http = require('http');

const SERVER_URL = process.env.SERVER_URL || 'http://127.0.0.1:17522';
const UPLOAD_TOKEN = process.env.UPLOAD_TOKEN || 'trae-pet-upload-token';
const RELEASE_DIR = path.join(__dirname, '..', 'release');

// Files to upload (electron-updater needs all three for differential updates)
const FILES_TO_UPLOAD = [
  'latest.yml',
  // Find the NSIS installer dynamically (name includes version)
];

function main() {
  if (!fs.existsSync(RELEASE_DIR)) {
    console.error('Error: release/ directory not found. Run `npm run dist` first.');
    process.exit(1);
  }

  // Find the installer .exe and .blockmap
  const allFiles = fs.readdirSync(RELEASE_DIR);
  const installer = allFiles.find((f) => f.match(/^TraePet-Setup-.+\.exe$/));
  const blockmap = allFiles.find((f) => f.match(/^TraePet-Setup-.+\.exe\.blockmap$/));

  const files = [...FILES_TO_UPLOAD];
  if (installer) files.push(installer);
  if (blockmap) files.push(blockmap);

  if (!installer) {
    console.error('Error: NSIS installer not found in release/. Run `npm run dist` first.');
    process.exit(1);
  }

  console.log(`Publishing to ${SERVER_URL}/publish`);
  console.log(`Files to upload: ${files.join(', ')}`);
  console.log('');

  uploadFiles(files, 0);
}

function uploadFiles(files, index) {
  if (index >= files.length) {
    console.log('');
    console.log('✅ All files uploaded successfully!');
    console.log(`   Update URL: ${SERVER_URL}/updates/latest.yml`);
    process.exit(0);
    return;
  }

  const filename = files[index];
  const filePath = path.join(RELEASE_DIR, filename);

  if (!fs.existsSync(filePath)) {
    console.warn(`⚠ Skipping ${filename} (not found)`);
    uploadFiles(files, index + 1);
    return;
  }

  const stat = fs.statSync(filePath);
  console.log(`Uploading ${filename} (${(stat.size / 1048576).toFixed(2)} MB)...`);

  const url = new URL('/publish', SERVER_URL);
  const options = {
    hostname: url.hostname,
    port: url.port,
    path: url.pathname,
    method: 'POST',
    headers: {
      'Authorization': `Bearer ${UPLOAD_TOKEN}`,
      'X-Filename': filename,
      'Content-Length': stat.size,
    },
  };

  const req = http.request(options, (res) => {
    let body = '';
    res.on('data', (chunk) => { body += chunk; });
    res.on('end', () => {
      if (res.statusCode === 200) {
        console.log(`  ✅ ${JSON.parse(body).filename} uploaded`);
      } else {
        console.error(`  ❌ Failed: ${res.statusCode} ${body}`);
        process.exit(1);
      }
      uploadFiles(files, index + 1);
    });
  });

  req.on('error', (err) => {
    console.error(`  ❌ Error: ${err.message}`);
    process.exit(1);
  });

  fs.createReadStream(filePath).pipe(req);
}

main();
