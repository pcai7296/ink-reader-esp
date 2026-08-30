#!/usr/bin/env node
// pc_stress_upload.js — PC 极限上传压力测试
// 复刻 phone 上传极限：扫描 编码(Content-Length vs chunked) x 大小 x 文件名,
// 每请求记录 HTTP status/body, 再用 /fs/list 验证文件是否真正落盘。
//
// 用法: node pc_stress_upload.js [deviceIp] [--chunked] [--cl] [--list-only] [--out dir]
// 默认 deviceIp=192.168.4.1。--chunked 只跑 chunked, --cl 只跑 Content-Length, 都缺省=两个都跑。
const http = require('http');
const fs = require('fs');
const path = require('path');

const DEV_IP = process.argv[2] || '192.168.4.1';
const PORT = 80;
const args = process.argv.slice(2);
const RUN_CHUNKED = args.includes('--chunked') || (!args.includes('--cl'));
const RUN_CONTENTLEN = args.includes('--cl') || (!args.includes('--chunked'));
const LIST_ONLY = args.includes('--list-only');
const OUT_DIR = args.includes('--out') ? args[args.indexOf('--out') + 1] : 'stress_results';

const BASE = `http://${DEV_IP}`;
if (!fs.existsSync(OUT_DIR)) fs.mkdirSync(OUT_DIR, { recursive: true });

// ---- HTTP helpers ----
function httpReq(method, reqPath, headers, body) {
  return new Promise((resolve) => {
    const opts = { host: DEV_IP, port: PORT, path: reqPath, method, headers: headers || {} };
    const req = http.request(opts, (res) => {
      let chunks = [];
      res.on('data', (d) => chunks.push(d));
      res.on('end', () => resolve({ status: res.statusCode, headers: res.headers, body: Buffer.concat(chunks).toString('utf8') }));
    });
    req.setTimeout(15000, () => { req.destroy(new Error('timeout')); });
    req.on('error', (e) => resolve({ status: 0, error: e.message }));
    if (body) { if (Buffer.isBuffer(body)) req.write(body); else req.write(body); }
    req.end();
  });
}

function randomBoundary() {
  return '----StressBoundary' + Math.random().toString(16).slice(2) + Date.now().toString(16);
}

function multipartBody(boundary, filename, fileBuf) {
  const pre = Buffer.from(
    `--${boundary}\r\n` +
    `Content-Disposition: form-data; name="data"; filename="${filename}"\r\n` +
    `Content-Type: text/plain\r\n\r\n`
  );
  const post = Buffer.from(`\r\n--${boundary}--\r\n`);
  return Buffer.concat([pre, fileBuf, post]);
}

// 逐小块写以触发 chunked（node 自动加 Transfer-Encoding: chunked, 不设 Content-Length）
function httpReqChunked(method, reqPath, headers, body) {
  return new Promise((resolve) => {
    const opts = { host: DEV_IP, port: PORT, path: reqPath, method, headers: headers || {} };
    const req = http.request(opts, (res) => {
      let chunks = [];
      res.on('data', (d) => chunks.push(d));
      res.on('end', () => resolve({ status: res.statusCode, headers: res.headers, body: Buffer.concat(chunks).toString('utf8') }));
    });
    req.setTimeout(6000, () => { req.destroy(new Error('timeout')); });
    req.on('error', (e) => resolve({ status: 0, error: e.message }));
    const CH = 512;
    for (let i = 0; i < body.length; i += CH) req.write(body.subarray(i, i + CH));
    req.end();
  });
}

async function uploadOne(caseName, filename, sizeBytes, chunked) {
  const boundary = randomBoundary();
  const fileBuf = Buffer.alloc(sizeBytes);
  // 可打印 ASCII 内容, 模拟英文小说
  let seed = 65;
  for (let i = 0; i < sizeBytes; i++) { fileBuf[i] = seed; if (++seed > 90) seed = 65; }
  const body = multipartBody(boundary, filename, fileBuf);
  const ct = `multipart/form-data; boundary=${boundary}`;
  const t0 = Date.now();
  let res;
  if (chunked) {
    res = await httpReqChunked('POST', '/fs/edit', { 'Content-Type': ct }, body);
  } else {
    res = await httpReq('POST', '/fs/edit', { 'Content-Type': ct, 'Content-Length': body.length }, body);
  }
  const ms = Date.now() - t0;
  // 验证落盘: GET /fs/file?path=<filename> 200=存在, 404=不存在
  const stat = await httpReq('GET', '/fs/file?path=' + encodeURIComponent(filename), {}, null);
  const exists = stat.status === 200;

  return {
    case: caseName, filename, size: sizeBytes, chunked,
    status: res.status, body: (res.body || res.error || '').slice(0, 120).replace(/\s+/g, ' ').trim(),
    ms, statStatus: stat.status, exists,
  };
}

async function main() {
  const results = [];
  const sizes = [26, 40, 100, 400, 800, 1400, 2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144, 524288, 1048576];
  const filenames = [
    ['root-basic', '/stress_basic.txt'],
    ['root-400b', '/stress_400b.txt'],
    ['sub', '/stress/sub_400.txt'],
    ['space', '/stress space name.txt'],
    ['unicode', '/stress_中文名.txt'],
  ];

  // 0) 建连探活
  const st = await httpReq('GET', '/fs/status', {}, null);
  console.log(`[probe] GET /fs/status -> ${st.status} ${(st.body||'').slice(0,80).replace(/\s+/g,' ')}`);

  // 1) 大小扫描（root 根 + Content-Length / chunked）
  for (const size of sizes) {
    if (RUN_CONTENTLEN) {
      const name = `/stress_sz_${size}_cl.txt`;
      const r = await uploadOne(`sz/${size}/cl`, name, size, false);
      results.push(r);
      console.log(`CL  sz=${size} -> HTTP ${r.status} ${r.body}  exists=${r.exists}  ${r.ms}ms`);
    }
    if (RUN_CHUNKED) {
      const name = `/stress_sz_${size}_ch.txt`;
      const r = await uploadOne(`sz/${size}/chunked`, name, size, true);
      results.push(r);
      console.log(`CH  sz=${size} -> HTTP ${r.status} ${r.body}  exists=${r.exists}  ${r.ms}ms  (timeout=${r.status===0} ${r.error||''})`);
    }
  }

  // 2) 文件名扫描（固定 400b）
  for (const [cname, fname] of filenames) {
    const base = fname.replace(/\.txt$/, '');
    if (RUN_CONTENTLEN) {
      const name = `${base}_cl.txt`;
      const r = await uploadOne(`name/${cname}/cl`, name, 400, false);
      results.push(r);
      console.log(`CL  name=${cname} -> HTTP ${r.status} ${r.body}  exists=${r.exists}`);
    }
    if (RUN_CHUNKED) {
      const name = `${base}_ch.txt`;
      const r = await uploadOne(`name/${cname}/chunked`, name, 400, true);
      results.push(r);
      console.log(`CH  name=${cname} -> HTTP ${r.status} ${r.body}  exists=${r.exists}`);
    }
  }

  // 3) 下载验证一个已落盘文件
  const dl = await httpReq('GET', '/fs/file?path=' + encodeURIComponent('/stress_sz_26_cl.txt') + '&download=true', {}, null);
  results.push({ case: 'download-check', status: dl.status, body: (dl.body||'').slice(0,60).replace(/\s+/g,' ') });
  console.log(`DL  /stress_sz_26_cl.txt -> HTTP ${dl.status} ${(dl.body||'').slice(0,60).replace(/\s+/g,' ')}`);

  // 汇总
  const fail = results.filter(r => r.status == null && r.status !== 200);
  const summary = {
    device: BASE, total: results.length,
    errors: results.filter(r => r.status === 0).map(r => `${r.case}: ${r.error}`),
    badPath: results.filter(r => r.status === 400 && /BAD PATH/i.test(r.body || '')).map(r => r.case),
    notExists: results.filter(r => r.status === 200 && !r.exists).map(r => r.case),
    non200: results.filter(r => r.status !== 200 && r.status !== 0).map(r => `${r.case}: HTTP ${r.status} ${r.body}`),
  };
  const outFile = path.join(OUT_DIR, `stress_${Date.now()}.json`);
  fs.writeFileSync(outFile, JSON.stringify({ summary, results }, null, 2));
  fs.writeFileSync(path.join(OUT_DIR, 'latest.json'), JSON.stringify({ summary, results }, null, 2));
  console.log('\n==== SUMMARY ====');
  console.log(`total=${summary.total} errors=${summary.errors.length} badPath=${summary.badPath.length} notExists(200 but absent)=${summary.notExists.length} non200=${summary.non200.length}`);
  if (summary.errors.length) console.log('errors:', summary.errors);
  if (summary.badPath.length) console.log('badPath:', summary.badPath);
  if (summary.notExists.length) console.log('200-but-no-file:', summary.notExists);
  if (summary.non200.length) console.log('non200:', summary.non200);
  console.log(`results -> ${outFile}`);
}

main().then(() => process.exit(0)).catch((e) => { console.error('FATAL', e); process.exit(1); });
