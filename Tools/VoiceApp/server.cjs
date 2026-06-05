const http = require('http');
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const PORT = 18790;
const HOST = '0.0.0.0';

// Message queues
let userMessages = [];     // voice UI → agent (picked up by CursorMCP check_messages proxy)
let agentReplies = [];     // agent → voice UI
let questionQueue = [];    // pending questions for voice UI
let clients = new Map();   // SSE clients

function broadcastSSE(event, data) {
  const msg = `event: ${event}\ndata: ${JSON.stringify(data)}\n\n`;
  for (const [id, res] of clients) {
    try { res.write(msg); } catch { clients.delete(id); }
  }
}

function handleAPI(req, res) {
  const url = new URL(req.url, `http://${req.headers.host}`);
  
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type');
  
  if (req.method === 'OPTIONS') {
    res.writeHead(204);
    res.end();
    return true;
  }

  // SSE endpoint for real-time updates
  if (url.pathname === '/api/events' && req.method === 'GET') {
    const clientId = crypto.randomUUID();
    res.writeHead(200, {
      'Content-Type': 'text/event-stream',
      'Cache-Control': 'no-cache',
      'Connection': 'keep-alive',
    });
    res.write(`event: connected\ndata: ${JSON.stringify({ id: clientId })}\n\n`);
    clients.set(clientId, res);
    req.on('close', () => clients.delete(clientId));
    return true;
  }

  // User sends a voice message (text after STT)
  if (url.pathname === '/api/send' && req.method === 'POST') {
    let body = '';
    req.on('data', c => body += c);
    req.on('end', () => {
      try {
        const { text, type } = JSON.parse(body);
        if (!text?.trim()) {
          res.writeHead(400);
          res.end(JSON.stringify({ error: 'empty message' }));
          return;
        }
        const msg = { id: crypto.randomUUID(), text: text.trim(), type: type || 'voice', ts: Date.now() };
        userMessages.push(msg);
        broadcastSSE('user_message', msg);
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ ok: true, id: msg.id }));
      } catch {
        res.writeHead(400);
        res.end(JSON.stringify({ error: 'invalid json' }));
      }
    });
    return true;
  }

  // CursorMCP proxy: check for pending user messages
  if (url.pathname === '/api/check' && req.method === 'GET') {
    const msgs = userMessages.splice(0);
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ messages: msgs }));
    return true;
  }

  // Agent pushes a reply (called by voice integration layer)
  if (url.pathname === '/api/reply' && req.method === 'POST') {
    let body = '';
    req.on('data', c => body += c);
    req.on('end', () => {
      try {
        const { text, markdown } = JSON.parse(body);
        const reply = { id: crypto.randomUUID(), text: text || '', markdown: markdown || '', ts: Date.now() };
        agentReplies.push(reply);
        broadcastSSE('agent_reply', reply);
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ ok: true }));
      } catch {
        res.writeHead(400);
        res.end(JSON.stringify({ error: 'invalid json' }));
      }
    });
    return true;
  }

  // Agent pushes a question
  if (url.pathname === '/api/question' && req.method === 'POST') {
    let body = '';
    req.on('data', c => body += c);
    req.on('end', () => {
      try {
        const data = JSON.parse(body);
        const q = { id: crypto.randomUUID(), ...data, ts: Date.now() };
        questionQueue.push(q);
        broadcastSSE('question', q);
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ ok: true, id: q.id }));
      } catch {
        res.writeHead(400);
        res.end(JSON.stringify({ error: 'invalid json' }));
      }
    });
    return true;
  }

  // Poll replies (fallback for non-SSE clients)
  if (url.pathname === '/api/replies' && req.method === 'GET') {
    const replies = agentReplies.splice(0);
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ replies }));
    return true;
  }

  // Health check
  if (url.pathname === '/api/health') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ ok: true, clients: clients.size, pendingMessages: userMessages.length }));
    return true;
  }

  return false;
}

function serveStatic(req, res) {
  const url = new URL(req.url, `http://${req.headers.host}`);
  let filePath = url.pathname === '/' ? '/index.html' : url.pathname;
  filePath = path.join(__dirname, filePath);
  
  const ext = path.extname(filePath);
  const mimeTypes = {
    '.html': 'text/html; charset=utf-8',
    '.js': 'application/javascript; charset=utf-8',
    '.css': 'text/css; charset=utf-8',
    '.json': 'application/json',
    '.png': 'image/png',
    '.svg': 'image/svg+xml',
    '.ico': 'image/x-icon',
  };

  fs.readFile(filePath, (err, data) => {
    if (err) {
      res.writeHead(404);
      res.end('Not Found');
      return;
    }
    res.writeHead(200, { 'Content-Type': mimeTypes[ext] || 'application/octet-stream' });
    res.end(data);
  });
}

const server = http.createServer((req, res) => {
  if (!handleAPI(req, res)) {
    serveStatic(req, res);
  }
});

server.listen(PORT, HOST, () => {
  console.log(`Voice App server running at http://${HOST}:${PORT}`);
  console.log(`Access from phone: http://10.147.17.208:${PORT}`);
});
