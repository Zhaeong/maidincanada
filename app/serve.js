// Minimal static server for the out/ directory (avoids needing Python).
const http = require('http');
const fs = require('fs');
const path = require('path');

const root = path.join(__dirname, 'out');
const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm' };

http.createServer((req, res) => {
  const file = path.join(root, req.url === '/' ? 'index.html' : req.url);
  fs.readFile(file, (err, data) => {
    if (err) { res.writeHead(404); res.end('not found'); return; }
    res.writeHead(200, { 'Content-Type': types[path.extname(file)] || 'application/octet-stream' });
    res.end(data);
  });
}).listen(8000, () => console.log('serving on http://localhost:8000'));
