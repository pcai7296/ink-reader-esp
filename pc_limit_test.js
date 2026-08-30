#!/usr/bin/env node
// pc_limit_test.js — 上传/下载极限压力测试（Content-Length 模式 = 前端真实路径）
// 目标: 实测"不崩溃、不失败"的最大上传/下载尺寸。
// 方法: 渐进式尺寸序列, 每档:
//   1) POST /fs/edit 上传 (multipart + Content-Length)
//   2) GET  /fs/file?path=...&download=true 下载并统计字节数
//   3) 前后探测 /fs/status 确认设备未崩溃/未复位
// 用法: node pc_limit_test.js [deviceIp] [--max-size <bytes>] [--min-size <bytes>] [--out <dir>]
// 示例: node pc_limit_test.js 192.168.4.1 --max-size 134217728
const http = require('http');
const fs = require('fs');
const path = require('path');

const DEV_IP = process.argv[2] || '192.168.4.1';
const PORT = 80;
const args = process.argv.slice(2);
const MAX_SIZE = args.includes('--max-size') ? parseInt(args[args.indexOf('--max-size') + 1], 10) : 64 * 1024 * 1024;
const MIN_SIZE = args.includes('--min-size') ? parseInt(args[args.indexOf('--min-size') + 1], 10) : 1024 * 1024;
const OUT_DIR = args.includes('--out') ? args[args.indexOf('--out') + 1] : 'stress_results';
if (!fs.existsSync(OUT_DIR)) fs.mkdirSync(OUT_DIR, { recursive: true });

// 吞吐基线估计 (0.2MB/s 曾测; 实际以实测为准) — 用于设置请求超时
function estTimeMs(bytes) { return Math.max(30000, (bytes / 200000) * 1000 * 3 + 20000); }

function httpReq(method, reqPath, headers, body, timeoutMs) {
  return new Promise((resolve) => {
    const opts = { host: DEV_IP, port: PORT, path: reqPath, method, headers: headers || {} };
    const req = http.request(opts, (res) => {
      const chunks = [];
      res.on('data', (d) => chunks.push(d));
      res.on('end', () => resolve({ status: res.statusCode, headers: res.headers, body: Buffer.concat(chunks) }));
    });
    req.setTimeout(timeoutMs || 20000, () => { req.destroy(new Error('timeout')); });
    req.on('error', (e) => resolve({ status: 0, error: e.message }));
    if (body) req.write(body);
    req.end();
  });
}

function randomBoundary() { return '----LimitBoundary' + Math.random().toString(16).slice(2) + Date.now().toString(16); }

function multipartBody(boundary, filename, fileBuf) {
  const pre = Buffer.from(
    `--${boundary}\r\n` +
    `Content-Disposition: form-data; name="data"; filename="${filename}"\r\n` +
    `Content-Type: application/octet-stream\r\n\r\n`
  );
  const post = Buffer.from(`\r\n--${boundary}--\r\n`);
  return Buffer.concat([pre, fileBuf, post]);
}

function fillPattern(buf) {
  // 可打印 ASCII 循环 + 每 1KB 插入 256B 伪随机, 便于下载字节数校验
  let seed = 65;
  for (let i = 0; i < buf.length; i++) {
    if ((i & 0x3FF) >= 0x300) { buf[i] = seed; if (++seed > 90) seed = 65; }
    else buf[i] = (i * 31 + (i >> 8)) & 0xFF;
  }
  return buf;
}

async function uploadOne(sizeBytes) {
  const name = `/limit_up_${sizeBytes}.bin`;
  const boundary = randomBoundary();
  const fileBuf = fillPattern(Buffer.alloc(sizeBytes));
  const body = multipartBody(boundary, name, fileBuf);
  const ct = `multipart/form-data; boundary=${boundary}`;
  const t0 = Date.now();
  const res = await httpReq('POST', '/fs/edit', { 'Content-Type': ct, 'Content-Length': body.length }, body, estTimeMs(body.length));
  const upMs = Date.now() - t0;
  return { name, size: sizeBytes, status: res.status, err: res.error || '', body: res.body ? res.body.toString('utf8').slice(0, 120).replace(/\s+/g, ' ') : '', upMs };
}

async function downloadVerify(name, expectedSize) {
  const t0 = Date.now();
  const res = await httpReq('GET', '/fs/file?path=' + encodeURIComponent(name) + '&download=true', {}, null, estTimeMs(expectedSize));
  const dlMs = Date.now() - t0;
  let bytes = 0;
  if (res.body) bytes = res.body.length;
  const ok = res.status === 200 && bytes === expectedSize;
  return { status: res.status, err: res.error || '', bytes, expect: expectedSize, ok, dlMs };
}

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

async function probe() {
  // 3 次重试, 间隔 2s: 区分瞬态(TCP 暂不可达) vs 真崩溃(连续 3 次失败)
  let last = '';
  for (let i = 0; i < 3; i++) {
    const res = await httpReq('GET', '/fs/status', {}, null, 8000);
    if (res.status === 200) return { tag: 'OK', raw: 'OK' };
    last = `DOWN(${res.status}${res.err ? ' ' + res.err : ''})`;
    await sleep(2000);
  }
  return { tag: last, raw: last };
}

async function deleteFile(name) {
  const res = await httpReq('DELETE', '/fs/edit?path=' + encodeURIComponent(name), {}, null, 15000);
  return res.status;
}

async function main() {
  console.log(`=== 上传/下载极限测试  device=${DEV_IP}  min=${MIN_SIZE}  max=${MAX_SIZE} ===`);
  const probe0 = await probe();
  console.log(`[probe0] /fs/status -> ${probe0.raw}`);
  if (probe0.tag !== 'OK') { console.log('设备不可达, 中止'); process.exit(1); }

  const results = [];
  const sizes = [];
  for (let s = MIN_SIZE; s <= MAX_SIZE; s *= 2) sizes.push(s);

  for (const size of sizes) {
    const before = await probe();
    console.log(`\n--- size=${size} (${(size / 1048576).toFixed(1)}MB)  device=${before.raw} ---`);
    if (before.tag !== 'OK') { console.log('设备不可达, 停止序列'); break; }

    const up = await uploadOne(size);
    console.log(`UP  ${up.name} -> HTTP ${up.status} ${up.body || up.err}  ${up.upMs}ms  (${(size / 1048576 / (up.upMs / 1000)).toFixed(2)}MB/s)`);
    results.push({ phase: 'upload', size, status: up.status, err: up.err, body: up.body, ms: up.upMs });

    if (up.status === 200) {
      const dl = await downloadVerify(up.name, size);
      console.log(`DL  ${up.name} -> HTTP ${dl.status} bytes=${dl.bytes}/${dl.expect} ok=${dl.ok}  ${dl.dlMs}ms  (${(size / 1048576 / (dl.dlMs / 1000)).toFixed(2)}MB/s)`);
      results.push({ phase: 'download', size, status: dl.status, err: dl.err, bytes: dl.bytes, expect: dl.expect, ok: dl.ok, ms: dl.dlMs });

      const del = await deleteFile(up.name);
      console.log(`DEL ${up.name} -> HTTP ${del}`);
      results.push({ phase: 'delete', size, status: del });
    } else {
      console.log('  (上传失败, 跳过下载/清理; 若为 507 空间不足则是物理上限)');
      if (up.status === 507) { console.log('  507 存储空间不足 → SD 空间耗尽, 停止序列'); break; }
    }

    const after = await probe();
    console.log(`     device-after=${after.raw}`);
    results[results.length - 1].deviceAfter = after.raw;
    if (after.tag !== 'OK') { console.log('设备持续不可达(疑似崩溃) → 停止序列'); break; }
  }

  // 汇总
  const upOK = results.filter(r => r.phase === 'upload' && r.status === 200);
  const dlOK = results.filter(r => r.phase === 'download' && r.ok);
  const summary = {
    device: DEV_IP,
    uploadMaxOK: upOK.length ? Math.max(...upOK.map(r => r.size)) : 0,
    downloadMaxOK: dlOK.length ? Math.max(...dlOK.map(r => r.size)) : 0,
    failures: results.filter(r => (r.phase === 'upload' && r.status !== 200) || (r.phase === 'download' && !r.ok))
      .map(r => `${r.phase} ${r.size}: ${r.status}${r.err ? ' ' + r.err : ''} ${r.body || ''}`),
    deviceDownAt: results.filter(r => r.deviceAfter && r.deviceAfter !== 'OK').map(r => `${r.phase} ${r.size}`),
    total: results.length,
  };
  const outFile = path.join(OUT_DIR, `limit_${Date.now()}.json`);
  fs.writeFileSync(outFile, JSON.stringify({ summary, results }, null, 2));
  fs.writeFileSync(path.join(OUT_DIR, 'limit_latest.json'), JSON.stringify({ summary, results }, null, 2));
  console.log('\n==== SUMMARY ====');
  console.log(`上传最大成功: ${summary.uploadMaxOK} bytes (${(summary.uploadMaxOK / 1048576).toFixed(1)}MB)`);
  console.log(`下载最大成功: ${summary.downloadMaxOK} bytes (${(summary.downloadMaxOK / 1048576).toFixed(1)}MB)`);
  if (summary.failures.length) { console.log('failures:'); summary.failures.forEach(f => console.log('  - ' + f)); }
  if (summary.deviceDownAt.length) console.log('device-down:', summary.deviceDownAt);
  console.log(`results -> ${outFile}`);
}

main().then(() => process.exit(0)).catch((e) => { console.error('FATAL', e); process.exit(1); });
