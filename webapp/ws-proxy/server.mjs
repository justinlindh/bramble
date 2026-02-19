/**
 * Bramble WS Proxy — WebSocket relay for device connections
 *
 * Bridges browser WebSocket connections (possibly behind TLS termination)
 * to plain ws:// connections on Bramble hardware devices.
 *
 * URL format: /proxy/<device-ip>
 * Example:    ws://localhost:3006/proxy/192.168.1.21
 *
 * The browser connects here; this service opens ws://<device-ip>/ws
 * and pipes frames bidirectionally. When either side closes, both close.
 *
 * Key design: the browser's WebSocket upgrade is held until the device
 * connection is established. This way the browser's transport.connect()
 * doesn't resolve until the full path (browser → proxy → device) is ready,
 * preventing RPC timeouts from the connect race.
 *
 * Designed to sit behind any TLS-terminating reverse proxy (Caddy, nginx, etc.)
 * so the browser can use wss:// while the device speaks plain ws://.
 */

import { createServer } from 'http';
import { WebSocketServer, WebSocket } from 'ws';

const PORT = parseInt(process.env.PORT || '3006', 10);

// Only allow RFC1918 + link-local targets — no proxying to the internet
const PRIVATE_IP = /^(10\.\d{1,3}\.\d{1,3}\.\d{1,3}|172\.(1[6-9]|2\d|3[01])\.\d{1,3}\.\d{1,3}|192\.168\.\d{1,3}\.\d{1,3}|169\.254\.\d{1,3}\.\d{1,3})$/;

const DEVICE_CONNECT_TIMEOUT_MS = 5000;

const wss = new WebSocketServer({ noServer: true });

const server = createServer((req, res) => {
  if (req.url === '/health') {
    res.writeHead(200, { 'Content-Type': 'text/plain' });
    res.end('ok');
    return;
  }
  res.writeHead(404);
  res.end('Not found');
});

// Handle upgrade manually: connect to device FIRST, then complete the browser upgrade
server.on('upgrade', (req, socket, head) => {
  const url = new URL(req.url, `http://${req.headers.host}`);
  const match = url.pathname.match(/^\/proxy\/([^/]+)$/);

  if (!match) {
    socket.write('HTTP/1.1 400 Bad Request\r\n\r\n');
    socket.destroy();
    return;
  }

  const targetIp = match[1];

  if (!PRIVATE_IP.test(targetIp)) {
    socket.write('HTTP/1.1 403 Forbidden\r\n\r\n');
    socket.destroy();
    return;
  }

  const deviceUrl = `ws://${targetIp}/ws`;
  console.log(`[proxy] connecting to device ${deviceUrl} before accepting browser…`);

  const deviceWs = new WebSocket(deviceUrl, {
    handshakeTimeout: DEVICE_CONNECT_TIMEOUT_MS,
  });

  deviceWs.on('open', () => {
    console.log(`[proxy] device ${targetIp} ready — completing browser upgrade`);

    // NOW accept the browser's WebSocket
    wss.handleUpgrade(req, socket, head, (browserWs) => {
      wss.emit('connection', browserWs, req, deviceWs, targetIp);
    });
  });

  deviceWs.on('error', (err) => {
    console.error(`[proxy] device ${targetIp} failed: ${err.message} — rejecting browser`);
    socket.write('HTTP/1.1 502 Bad Gateway\r\n\r\n');
    socket.destroy();
  });

  // If the browser hangs up before device connects
  socket.on('close', () => {
    if (deviceWs.readyState === WebSocket.CONNECTING) {
      deviceWs.close();
    }
  });
});

// Wire up the bidirectional relay once both sides are ready
wss.on('connection', (browserWs, req, deviceWs, targetIp) => {
  console.log(`[proxy] relay active: browser ↔ ${targetIp}`);

  // Device → Browser
  deviceWs.on('message', (data, isBinary) => {
    if (browserWs.readyState === WebSocket.OPEN) {
      browserWs.send(data, { binary: isBinary });
    }
  });

  // Browser → Device
  browserWs.on('message', (data, isBinary) => {
    if (deviceWs.readyState === WebSocket.OPEN) {
      deviceWs.send(data, { binary: isBinary });
    }
  });

  deviceWs.on('close', (code, reason) => {
    console.log(`[proxy] device ${targetIp} closed (${code})`);
    if (browserWs.readyState === WebSocket.OPEN) {
      browserWs.close(code || 1000, reason || 'Device disconnected');
    }
  });

  deviceWs.on('error', (err) => {
    console.error(`[proxy] device ${targetIp} error: ${err.message}`);
    if (browserWs.readyState === WebSocket.OPEN) {
      browserWs.close(4502, `Device error: ${err.message}`);
    }
  });

  browserWs.on('close', () => {
    console.log(`[proxy] browser disconnected (was → ${targetIp})`);
    if (deviceWs.readyState === WebSocket.OPEN || deviceWs.readyState === WebSocket.CONNECTING) {
      deviceWs.close();
    }
  });

  browserWs.on('error', (err) => {
    console.error(`[proxy] browser error: ${err.message}`);
    if (deviceWs.readyState === WebSocket.OPEN) {
      deviceWs.close();
    }
  });
});

server.listen(PORT, () => {
  console.log(`[ws-proxy] listening on :${PORT}`);
});
