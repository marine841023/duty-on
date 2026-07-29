/**
 * Download assets (Live2D Cubism Core SDK) for offline use.
 * Runs automatically after npm install.
 */

const https = require('https');
const fs = require('fs');
const path = require('path');

const assetsDir = path.join(__dirname, '..', 'assets');
const libsDir = path.join(assetsDir, 'libs');
const live2dDir = path.join(assetsDir, 'live2d');

// Create directories
fs.mkdirSync(libsDir, { recursive: true });
fs.mkdirSync(live2dDir, { recursive: true });

const files = [
  {
    url: 'https://cubism.live2d.com/sdk-web/cubismcore/live2dcubismcore.min.js',
    dest: path.join(libsDir, 'live2dcubismcore.min.js'),
    name: 'Live2D Cubism Core SDK',
  },
];

function download(url, dest) {
  return new Promise((resolve, reject) => {
    const file = fs.createWriteStream(dest);
    const request = (url) => {
      https.get(url, (response) => {
        // Follow redirects
        if (response.statusCode === 301 || response.statusCode === 302) {
          response.resume();
          request(response.headers.location);
          return;
        }
        if (response.statusCode !== 200) {
          reject(new Error(`HTTP ${response.statusCode}`));
          return;
        }
        response.pipe(file);
        file.on('finish', () => {
          file.close();
          resolve();
        });
      }).on('error', (err) => {
        fs.unlink(dest, () => {});
        reject(err);
      });
    };
    request(url);
  });
}

async function main() {
  console.log('[assets] Checking required assets...');

  for (const file of files) {
    if (fs.existsSync(file.dest)) {
      console.log(`[assets] ${file.name} already exists, skipping.`);
      continue;
    }
    try {
      console.log(`[assets] Downloading ${file.name}...`);
      await download(file.url, file.dest);
      console.log(`[assets] Downloaded: ${path.basename(file.dest)}`);
    } catch (err) {
      console.warn(`[assets] Warning: Failed to download ${file.name}: ${err.message}`);
      console.warn(`[assets] The app will try CDN fallback at runtime.`);
    }
  }

  console.log('[assets] Done.');
}

main();
