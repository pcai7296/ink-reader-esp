// node_dl64_check.js — Node 原生下载 64MB 校验 (对照: 判断 Chrome 下载崩溃是否固件/AP 问题)
const http = require('http');
const DEV = '192.168.4.1';
const PATH = '/up_64m.txt';
const SIZE = 64 * 1024 * 1024;

const t0 = Date.now();
const req = http.request({ host: DEV, port: 80, path: '/fs/file?path=' + encodeURIComponent(PATH) + '&download=true', method: 'GET' }, (res) => {
  let bytes = 0;
  res.on('data', (d) => { bytes += d.length; });
  res.on('end', () => {
    const ms = Date.now() - t0;
    const ok = res.statusCode === 200 && bytes === SIZE;
    console.log(`Node 下载 ${PATH} -> HTTP ${res.statusCode} bytes=${bytes}/${SIZE} ${ok ? 'OK' : 'FAIL'} ${ms}ms (${(SIZE/1048576/(ms/1000)).toFixed(2)}MB/s)`);
    process.exit(ok ? 0 : 1);
  });
});
req.setTimeout(600000, () => { console.log('timeout'); process.exit(1); });
req.on('error', (e) => { console.log('ERR', e.message); process.exit(1); });
req.end();