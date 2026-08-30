// up_status_test.js — 真实 Chrome 上传, 验证墨水屏上传状态(上传中/上传完毕)不崩溃
const { chromium } = require('playwright-core');
const fs = require('fs');
const DEV = 'http://' + (process.argv[2] || '192.168.4.1');
const TDIR = 'C:\\Users\\Administrator\\AppData\\Local\\Temp\\dsb_upload_test';
const SIZE = 200 * 1024;  // 200KB

(async () => {
  if (!fs.existsSync(TDIR)) fs.mkdirSync(TDIR, { recursive: true });
  const p = TDIR + '\\up_status.txt';
  let seed = 65; const buf = Buffer.alloc(SIZE);
  for (let i = 0; i < SIZE; i++) { buf[i] = seed; if (++seed > 90) seed = 65; }
  fs.writeFileSync(p, buf);

  const browser = await chromium.launch({
    executablePath: 'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe',
    headless: true, args: ['--no-first-run', '--disable-gpu', '--window-size=390,900']
  });
  const ctx = await browser.newContext({ viewport: { width: 390, height: 844 }, isMobile: true, hasTouch: true, userAgent: 'Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Mobile Safari/537.36' });
  const page = await ctx.newPage();
  await page.goto(DEV + '/fs/edit', { waitUntil: 'domcontentloaded', timeout: 20000 });
  await page.waitForTimeout(2000);

  console.log('=== 上传 200KB ===');
  await page.setInputFiles('input[type=file]', p);
  await page.waitForTimeout(300);
  await page.evaluate(() => {
    const btn = Array.from(document.querySelectorAll('button')).find(b => b.textContent.trim() === '上传');
    if (btn) btn.click();
  });
  // 等 POST 响应
  const resp = await page.waitForResponse(r => r.url().includes('/edit') && r.request().method() === 'POST', { timeout: 30000 });
  console.log('POST /fs/edit ->', resp.status(), await resp.text().catch(()=>''));
  await page.waitForTimeout(2500);   // 等墨水屏恢复配网页

  // 验证落盘
  const exists = await page.evaluate(async (u) => { const r = await fetch(u); return r.status; }, DEV + '/fs/file?path=' + encodeURIComponent('/up_status.txt'));
  console.log('落盘 /up_status.txt ->', exists);

  // 清理
  await page.evaluate(async (u) => { await fetch(u, { method: 'DELETE' }); }, DEV + '/fs/edit?path=' + encodeURIComponent('/up_status.txt'));
  await page.screenshot({ path: 'up_status_result.png' });
  await browser.close();
  console.log('=== DONE ===');
})().catch(e => { console.error('FATAL:', e); process.exit(1); });
