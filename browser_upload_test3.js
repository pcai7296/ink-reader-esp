// browser_upload_test3.js — 走真实 manager.htm (新 Blob 上传路径) 端到端上传验证
// 用真实 Chrome 移动模拟: 打开 /fs/edit → 选择文件 → 点"上传" → 捕获 POST 响应 + 请求头(有无 Content-Length/chunked) → 验证落盘。
// 用法: node browser_upload_test3.js [deviceIp]
const { chromium } = require('playwright-core');
const fs = require('fs');

const DEV = 'http://' + (process.argv[2] || '192.168.4.1');
const TDIR = 'C:\\Users\\Administrator\\AppData\\Local\\Temp\\dsb_bro_upload';

function makeFile(name, size) {
  if (!fs.existsSync(TDIR)) fs.mkdirSync(TDIR, { recursive: true });
  const p = TDIR + '\\' + name;
  let seed = 65;
  const buf = Buffer.alloc(size);
  for (let i = 0; i < size; i++) { buf[i] = seed; if (++seed > 90) seed = 65; }
  fs.writeFileSync(p, buf);
  return p;
}

async function uploadOne(browser, page, path, size, label) {
  const filePath = makeFile('up_' + size + '_' + label + '.txt', size);
  const seenRequest = { headers: null, method: null };
  let postResult = null;
  page.removeAllListeners('request');
  page.removeAllListeners('response');
  page.on('request', (r) => {
    if (r.url().includes('/edit') && r.method() === 'POST') {
      seenRequest.method = r.method();
      seenRequest.headers = r.headers();   // 检查 Content-Length / Transfer-Encoding
    }
  });
  const postRespPromise = page.waitForResponse((r) => r.url().includes('/edit') && r.request().method() === 'POST', { timeout: 15000 }).catch(() => null);
  page.on('response', (r) => {
    if (r.url().includes('/edit') && r.request().method() === 'POST') postResult = r;
  });

  await page.setInputFiles('input[type=file]', filePath);
  await page.waitForTimeout(300);
  // 点"上传"按钮（第一个 innerHTML='上传' 的 button）
  await page.evaluate(() => {
    const btn = Array.from(document.querySelectorAll('button')).find(b => b.textContent.trim() === '上传');
    if (btn) btn.click();
  });
  const resp = await postRespPromise;
  await page.waitForTimeout(700);

  let respStatus = null, respBody = null;
  if (resp) { respStatus = resp.status(); respBody = await resp.text().catch(()=>null); }
  const cl = seenRequest.headers && seenRequest.headers['content-length'];
  const te = seenRequest.headers && (seenRequest.headers['transfer-encoding'] || '');
  const clVal = cl || null;

  // 验证落盘: GET /fs/file?path=... 200=存在
  const fileExists = await playwrightGet(page, '/fs/file?path=' + encodeURIComponent(path));
  console.log(`[${label}] size=${size} POST(/fs/edit) status=${respStatus} body="${(respBody||'').slice(0,40).replace(/\s+/g,' ')}" Content-Length=${clVal} chunked=${te} 落盘=${fileExists}`);
  return { label, size, respStatus, respBody, contentLength: clVal, chunked: te, exists: fileExists };
}

async function playwrightGet(page, q) {
  try {
    const r = await page.evaluate(async (url) => {
      const res = await fetch(url);
      return res.status;
    }, DEV + q);
    return r === 200;
  } catch (e) { return false; }
}

(async () => {
  const browser = await chromium.launch({
    executablePath: 'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe',
    headless: false,
    args: ['--no-first-run', '--disable-infobars', '--window-size=390,900']
  });
  const context = await browser.newContext({
    viewport: { width: 390, height: 844 },
    isMobile: true, hasTouch: true,
    userAgent: 'Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Mobile Safari/537.36'
  });
  const page = await context.newPage();

  console.log('=== open /fs/edit ===');
  const resp = await page.goto(DEV + '/fs/edit', { waitUntil: 'domcontentloaded', timeout: 15000 }).catch(e => { console.log('goto FAIL', e.message); return null; });
  if (!resp) { await browser.close(); process.exit(1); }
  console.log('status', resp.status(), 'title', await page.title());
  await page.waitForTimeout(1500);
  console.log('uploadMode =', await page.evaluate(() => window.__dsb_uploadMode));

  const results = [];
  // 复刻手机失败场景: 400B; 再加上其它大小
  for (const [size, label] of [[26,'s26'],[400,'s400'],[4096,'s4k'],[65536,'s64k']]) {
    const path = `/broupload_${label}.txt`;
    results.push(await uploadOne(browser, page, path, size, label));
  }

  console.log('\n==== SUMMARY ====');
  let bad = 0;
  for (const r of results) {
    const ok = r.respStatus === 200 && r.exists;
    if (!ok) bad++;
    console.log(`  ${r.label}: HTTP ${r.respStatus} exists=${r.exists} CL=${r.contentLength} chunked=${r.chunked} ${ok ? 'OK' : 'FAIL'}`);
  }
  console.log(`failures=${bad}/${results.length}`);
  await page.screenshot({ path: 'upload_result3.png' });
  await browser.close();
  console.log('=== DONE ===');
  process.exit(bad ? 1 : 0);
})().catch(e => { console.error('FATAL:', e); process.exit(1); });
