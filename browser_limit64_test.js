// browser_limit64_test.js — 真实 Chrome(移动模拟, 无头) 端到端 64MB 上传 + 下载极限验证
// 走真实 manager.htm 前端(Blob 强制 Content-Length 路径): 打开 /fs/edit → 选 64MB 文件 → 点上传 →
// 捕获 POST 响应 → 落盘字节数校验 → 下载 64MB 字节数校验。
// 用法: node browser_limit64_test.js [deviceIp]  (须在 %TEMP%\dsh_browser_test\ 下运行, node_modules 在那)
const { chromium } = require('playwright-core');
const fs = require('fs');

const DEV = 'http://' + (process.argv[2] || '192.168.4.1');
const TDIR = 'C:\\Users\\Administrator\\AppData\\Local\\Temp\\dsb_bro_upload';
const SIZE = 64 * 1024 * 1024;   // 64MB
const UP_TIMEOUT = 600000;       // 64MB @0.14MB/s ≈ 8min; 留足余量
const DL_TIMEOUT = 600000;       // 64MB @0.18MB/s ≈ 6min

function makeFile(name, size) {
  if (!fs.existsSync(TDIR)) fs.mkdirSync(TDIR, { recursive: true });
  const p = TDIR + '\\' + name;
  let seed = 65;
  const buf = Buffer.alloc(size);
  for (let i = 0; i < size; i++) { buf[i] = seed; if (++seed > 90) seed = 65; }
  fs.writeFileSync(p, buf);
  return p;
}

(async () => {
  const browser = await chromium.launch({
    executablePath: 'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe',
    headless: true,               // ★ 无头运行, 不弹窗
    args: ['--no-first-run', '--disable-infobars', '--window-size=390,900', '--disable-gpu']
  });
  const context = await browser.newContext({
    viewport: { width: 390, height: 844 },
    isMobile: true, hasTouch: true,
    userAgent: 'Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Mobile Safari/537.36',
    acceptDownloads: true,
  });
  const page = await context.newPage();
  page.setDefaultTimeout(UP_TIMEOUT);

  console.log('=== open /fs/edit ===');
  const resp = await page.goto(DEV + '/fs/edit', { waitUntil: 'domcontentloaded', timeout: 20000 }).catch(e => { console.log('goto FAIL', e.message); return null; });
  if (!resp) { await browser.close(); process.exit(1); }
  console.log('status', resp.status(), 'title', await page.title());
  await page.waitForTimeout(1500);
  console.log('uploadMode =', await page.evaluate(() => window.__dsb_uploadMode));

  // ============ 64MB 上传 ============
  // ⚠️ 上传路径 = 前端 pathInput 值: 选择文件后 manager.htm 自动把 pathInput 设为本地文件名
  // (L186-187), 故真实落盘路径是 /up_64m.txt 而非脚本猜测。上传前先读 pathInput 真实值。
  const upPath = await page.evaluate(() => {
    const pi = document.getElementById('pathInput');
    return pi ? pi.value : null;
  });
  console.log('pathInput =', upPath, '(前端决定真实上传路径)');
  const filePath = makeFile('up_64m.txt', SIZE);
  let seenRequest = null;
  let postResult = null;
  page.on('request', (r) => {
    if (r.url().includes('/edit') && r.method() === 'POST') {
      seenRequest = { method: r.method(), headers: r.headers() };
    }
  });
  page.on('response', (r) => {
    if (r.url().includes('/edit') && r.request().method() === 'POST') postResult = r;
  });

  console.log(`=== 上传 64MB (${SIZE} bytes) ===`);
  const t0 = Date.now();
  await page.setInputFiles('input[type=file]', filePath);
  await page.waitForTimeout(500);
  await page.evaluate(() => {
    const btn = Array.from(document.querySelectorAll('button')).find(b => b.textContent.trim() === '上传');
    if (btn) btn.click();
  });
  // 等待 POST 响应（长超时）
  let respStatus = null, respBody = null;
  try {
    const pr = await page.waitForResponse((r) => r.url().includes('/edit') && r.request().method() === 'POST', { timeout: UP_TIMEOUT });
    respStatus = pr.status();
    respBody = await pr.text().catch(() => null);
  } catch (e) {
    console.log('POST response wait FAIL:', e.message.slice(0, 120));
  }
  const upMs = Date.now() - t0;
  const cl = seenRequest && seenRequest.headers && seenRequest.headers['content-length'];
  const te = seenRequest && seenRequest.headers && (seenRequest.headers['transfer-encoding'] || '');
  // 上传后 pathInput 可能被前端改过(选择文件时 set 为文件名); 以最终值为准
  const finalPath = await page.evaluate(() => {
    const pi = document.getElementById('pathInput');
    return pi ? pi.value : null;
  });
  const realPath = (finalPath && finalPath !== '/' && finalPath.trim() !== '') ? finalPath : upPath;
  console.log(`POST /fs/edit -> HTTP ${respStatus} body="${(respBody||'').slice(0,60).replace(/\s+/g,' ')}" CL=${cl||null} chunked=${te||''} ${upMs}ms (${(SIZE/1048576/(upMs/1000)).toFixed(2)}MB/s)`);
  console.log('真实上传路径 =', realPath);
  await page.waitForTimeout(500);

  // 落盘字节数校验: fetch 读完整文件数长度
  console.log('=== 落盘校验 ===');
  let upBytes = 0;
  try {
    upBytes = await page.evaluate(async (url) => {
      const r = await fetch(url);
      if (!r.ok) return -1;
      const b = await r.arrayBuffer();
      return b.byteLength;
    }, DEV + '/fs/file?path=' + encodeURIComponent(realPath));
  } catch (e) { console.log('verify FAIL', e.message.slice(0, 100)); }
  console.log(`落盘 /fs/file ${realPath} -> ${upBytes} bytes (期望 ${SIZE}) ${upBytes === SIZE ? 'OK' : 'FAIL'}`);

  // ============ 64MB 下载 ============
  console.log(`=== 下载 64MB ===`);
  let dlStatus = null, dlBytes = 0, dlMs = 0;
  try {
    const dl = await page.evaluate(async (url) => {
      const t = performance.now();
      const r = await fetch(url);
      const b = await r.arrayBuffer();
      return { status: r.status, bytes: b.byteLength, ms: Math.round(performance.now() - t) };
    }, DEV + '/fs/file?path=' + encodeURIComponent(realPath) + '&download=true');
    dlStatus = dl.status; dlBytes = dl.bytes; dlMs = dl.ms;
  } catch (e) { console.log('download FAIL', e.message.slice(0, 100)); }
  console.log(`下载 /fs/file?download=true -> HTTP ${dlStatus} bytes=${dlBytes} (期望 ${SIZE}) ${dlBytes === SIZE ? 'OK' : 'FAIL'} ${dlMs}ms (${(SIZE/1048576/(dlMs/1000)).toFixed(2)}MB/s)`);

  // ============ 清理测试文件 ============
  console.log('=== 清理 ===');
  try {
    const del = await page.evaluate(async (url) => {
      const r = await fetch(url, { method: 'DELETE' });
      return r.status;
    }, DEV + '/fs/edit?path=' + encodeURIComponent(realPath));
    console.log('DELETE', realPath, '->', del);
  } catch (e) { console.log('del FAIL', e.message.slice(0, 100)); }
  await page.waitForTimeout(300);

  // 汇总
  const upOk = respStatus === 200 && upBytes === SIZE;
  const dlOk = dlStatus === 200 && dlBytes === SIZE;
  console.log('\n==== SUMMARY ====');
  console.log(`上传64MB: HTTP ${respStatus} 落盘${upBytes}/${SIZE} ${upOk ? 'OK' : 'FAIL'} (CL=${cl||'?'} chunked=${te||'无'})`);
  console.log(`下载64MB: HTTP ${dlStatus} 字节${dlBytes}/${SIZE} ${dlOk ? 'OK' : 'FAIL'}`);
  console.log(`failures=${(upOk && dlOk) ? 0 : 2}/2`);
  await page.screenshot({ path: 'browser_limit64_result.png' });
  await browser.close();
  console.log('=== DONE ===');
  process.exit((upOk && dlOk) ? 0 : 1);
})().catch(e => { console.error('FATAL:', e); process.exit(1); });
