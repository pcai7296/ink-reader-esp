// browser_dl64_test.js — 真实 Chrome(移动模拟, 无头) 下载 64MB 验证
// 走 manager.htm 真实下载路径: iframe src = file?path=...&download=true → Chrome 原生下载事件。
// 用 Playwright download 事件捕获(浏览器持续消费流, 不卡), 校验下载字节数。
// 前置: 64MB 文件已由 browser_limit64_test.js 上传到 SD (/up_64m.txt 67108864 字节)。
// 用法: node browser_dl64_test.js [deviceIp]  (须在 %TEMP%\dsh_browser_test\ 下运行)
const { chromium } = require('playwright-core');

const DEV = 'http://' + (process.argv[2] || '192.168.4.1');
const PATH = '/up_64m.txt';
const SIZE = 64 * 1024 * 1024;
const DL_TIMEOUT = 600000;

(async () => {
  const browser = await chromium.launch({
    executablePath: 'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe',
    headless: true,
    args: ['--no-first-run', '--disable-infobars', '--window-size=390,900', '--disable-gpu']
  });
  const context = await browser.newContext({
    viewport: { width: 390, height: 844 },
    isMobile: true, hasTouch: true,
    userAgent: 'Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Mobile Safari/537.36',
    acceptDownloads: true,
  });
  const page = await context.newPage();
  page.setDefaultTimeout(DL_TIMEOUT);

  console.log('=== open /fs/edit ===');
  const resp = await page.goto(DEV + '/fs/edit', { waitUntil: 'domcontentloaded', timeout: 20000 }).catch(e => { console.log('goto FAIL', e.message); return null; });
  if (!resp) { await browser.close(); process.exit(1); }
  await page.waitForTimeout(1500);

  // 先确认文件存在
  const exists = await page.evaluate(async (url) => {
    const r = await fetch(url);
    return r.status;
  }, DEV + '/fs/file?path=' + encodeURIComponent(PATH));
  console.log(`预检 /fs/file ${PATH} -> HTTP ${exists} (期望 200)`);
  if (exists !== 200) { await browser.close(); process.exit(1); }

  // 触发下载: 设 iframe src (manager.htm loadDownload 同款)
  console.log('=== 触发下载 (iframe src) ===');
  const dlPromise = page.waitForEvent('download', { timeout: DL_TIMEOUT });
  await page.evaluate((url) => {
    const f = document.createElement('iframe');
    f.id = 'dl-test-frame';
    f.style.display = 'none';
    document.body.appendChild(f);
    f.src = url;
  }, DEV + '/fs/file?path=' + encodeURIComponent(PATH) + '&download=true');

  let dl = null;
  const t0 = Date.now();
  try {
    dl = await dlPromise;
  } catch (e) {
    console.log('download event wait FAIL:', e.message.slice(0, 120));
  }
  if (!dl) { await browser.close(); process.exit(1); }

  console.log('download event fired:', dl.suggestedFilename());

  // 等待下载完成并读字节数
  let dlPath = null;
  try {
    dlPath = await dl.path();   // 等下载完成返回临时文件路径
  } catch (e) {
    console.log('dl.path FAIL:', e.message.slice(0, 120));
  }
  const dlMs = Date.now() - t0;

  let bytes = 0, ok = false;
  if (dlPath) {
    const fs = require('fs');
    const st = fs.statSync(dlPath);
    bytes = st.size;
    ok = bytes === SIZE;
    console.log(`下载完成: ${bytes} bytes (期望 ${SIZE}) ${ok ? 'OK' : 'FAIL'} ${dlMs}ms (${(SIZE/1048576/(dlMs/1000)).toFixed(2)}MB/s)`);
    // 清理临时下载
    try { fs.unlinkSync(dlPath); } catch (e) {}
  } else {
    console.log('download path unavailable (下载未完成?)');
  }

  console.log('\n==== SUMMARY ====');
  console.log(`下载64MB: ${ok ? 'OK' : 'FAIL'} bytes=${bytes}/${SIZE} ${dlMs}ms`);
  await page.screenshot({ path: 'browser_dl64_result.png' });
  await browser.close();
  console.log('=== DONE ===');
  process.exit(ok ? 0 : 1);
})().catch(e => { console.error('FATAL:', e); process.exit(1); });
