#!/usr/bin/env node

const fsSync = require('fs');
const path = require('path');
const http = require('http');
const crypto = require('crypto');
const readline = require('readline');

const rl = readline.createInterface({
  input: process.stdin,
  output: process.stdout
});

// Default configs
const DEFAULT_IP = '192.168.11.144'; // Default IP address of the ESP32
const SPIFFS_BIN_PATH = '.pio/build/esp32dev/spiffs.bin'; // Path to the SPIFFS binary

function calculateMD5(filePath) {
  return new Promise((resolve, reject) => {
    const hash = crypto.createHash('md5');
    const stream = fsSync.createReadStream(filePath);

    stream.on('data', (data) => hash.update(data));
    stream.on('end', () => resolve(hash.digest('hex'))); // "base64" | "base64url" | "hex" | "binary"
    stream.on('error', (error) => reject(error));
  });
}

function startOTA(ip, md5) {
  return new Promise((resolve, reject) => {
    const url = `http://${ip}/ota/start?mode=fs&hash=${md5}`;

    http.get(url, (res) => {
      let data = '';

      res.on('data', (chunk) => {
        data += chunk;
      });

      res.on('end', () => {
        if (res.statusCode === 200) {
          console.log('OTA update started successfully');
          resolve();
        } else {
          reject(new Error(`Failed to start OTA update: ${data}`));
        }
      });
    }).on('error', (err) => {
      reject(new Error(`Error starting OTA update: ${err.message}`));
    });
  });
}

function performOTA(ip, filePath) {
  return new Promise((resolve, reject) => {
    const boundary = '----WebKitFormBoundary' + Math.random().toString(16).slice(2);
    // const auth = Buffer.from(`${USERNAME}:${PASSWORD}`).toString('base64');
    const filename = path.basename(filePath);
    const fileSize = fsSync.statSync(filePath).size;
    const fileStream = fsSync.createReadStream(filePath);

    const preamble = Buffer.from(
      `--${boundary}\r\n` +
      `Content-Disposition: form-data; name="file"; filename="${filename}"\r\n` +
      `Content-Type: application/macbinary\r\n\r\n`
    );

    const postamble = Buffer.from(`\r\n--${boundary}--\r\n`);

    const contentLength = preamble.length + fileSize + postamble.length;

    const options = {
      hostname: ip,
      // port: PORT,
      path: '/ota/upload',
      method: 'POST',
      headers: {
        // 'Authorization': `Basic ${auth}`,
        'Content-Type': `multipart/form-data; boundary=${boundary}`,
        'Content-Length': contentLength,
        'Connection': 'close'
      },
    };

    const req = http.request(options, res => {
      let responseData = '';
      res.on('data', chunk => responseData += chunk.toString());
      res.on('end', () => {
        if (res.statusCode === 200) {
          console.log('Upload OK:', responseData);
          resolve();
        } else {
          reject(new Error(`Upload failed: ${res.statusCode}, ${responseData}`));
        }
      });
    });

    req.on('error', reject);

    // Write preamble, file content, and postamble
    req.write(preamble);
    fileStream.pipe(req, { end: false });
    fileStream.on('end', () => req.end(postamble));
  });
}

async function main() {
  try {

    if (!fsSync.existsSync(SPIFFS_BIN_PATH)) {
      console.error(`SPIFFS binary not found at ${SPIFFS_BIN_PATH}`);
      console.log('Please build the SPIFFS image first with: pio run --target buildfs');
      process.exit(1);
    }

    const ip = await new Promise((resolve) => {
      rl.question(`Enter ESP32 IP address [${DEFAULT_IP}]: `, (answer) => {
        resolve(answer.trim() || DEFAULT_IP);
      });
    });

    console.log(`Uploading SPIFFS to ${ip}...`);

    const md5 = await calculateMD5(SPIFFS_BIN_PATH);

    console.log(`Calculating MD5 hash MD5 hash: ${md5}`);

    await startOTA(ip, md5);

    await performOTA(ip, SPIFFS_BIN_PATH);

    console.log('SPIFFS OTA update completed successfully!');
    console.log('The device will restart automatically.');
  } catch (error) {
    console.error(`Error: ${error.message}`);
    process.exit(1);
  } finally {
    rl.close();
  }
}

main();
