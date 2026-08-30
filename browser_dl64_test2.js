// browser_dl64_test2.js — 真实 Chrome(移动模拟, 无头) 下载 64MB 验证 v2
// 方式: 独立 page 直接 goto 下载 URL (Content-Disposition: attachment) → Playwright download 事件
// (避开 iframe 在无头移动模拟下被 cancel 的问题)。校验下载字节数。
// 前置: 64MB 文件已上传到 SD (/up_64m.txt)。
// 用法: node browser_dl64_test2.js [deviceIp]  (须在 %TEMP%\dsh_browser_test\ 下运行)
const { chromium } = require('playwright-core');
const fs = require('fs');

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
    // 移动 UA 保留, 但 isMobile:false — 无头移动模拟(isMobile)下大文件下载会被 Chrome 取消(canceled)
    isMobile: false, hasTouch: false,
    userAgent: 'Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Mobile Safari/537.36',
    acceptDownloads: true,
    downloadsPath: 'C:\\Users\\Administrator\\AppData\\Local\\Temp\\dsb_bro_upload\\dl64',
  });

  // 文件存在性已由 /fs/list 确认 (67108864 字节); 不再预检 (GET+cancel 会触发设备断连保护)

  // 独立 page 触发下载 (goto attachment URL)
  console.log('=== 触发下载 (goto download URL) ===');
  const dlPage = await context.newPage();
  const dlPromise = dlPage.waitForEvent('download', { timeout: DL_TIMEOUT });
  const t0 = Date.now();
  let dl = null;
  try {
    // goto 后立即返回; 下载由 Content-Disposition 触发, 不等页面加载
    await dlPage.goto(DEV + '/fs/file?path=' + encodeURIComponent(PATH) + '&download=true', { waitUntil: 'commit', timeout: 15000 }).catch(() => {});
    dl = await dlPromise;
  } catch (e) {
    console.log('download wait FAIL:', e.message.slice(0, 160));
  }
  if (!dl) { await browser.close(); process.exit(1); }

  console.log('download event fired:', dl.suggestedFilename());

  // 等下载完成
  let dlPath = null;
  try {
    dlPath = await dl.path();
  } catch (e) {
    console.log('dl.path FAIL:', e.message.slice(0, 120));
  }
  const dlMs = Date.now() - t0;

  let bytes = 0, ok = false;
  if (dlPath && fs.existsSync(dlPath)) {
    bytes = fs.statSync(dlPath).size;
    ok = bytes === SIZE;
    console.log(`下载完成: ${bytes} bytes (期望 ${SIZE}) ${ok ? 'OK' : 'FAIL'} ${dlMs}ms (${(SIZE/1048576/(dlMs/1000)).toFixed(2)}MB/s)`);
    try { fs.unlinkSync(dlPath); } catch (e) {}
  } else {
    console.log('download path unavailable (下载未完成或被取消)');
  }

  console.log('\n==== SUMMARY ====');
  console.log(`下载64MB: ${ok ? 'OK' : 'FAIL'} bytes=${bytes}/${SIZE} ${dlMs}ms`);
  await dlPage.screenshot({ path: 'browser_dl64_result2.png' });
  await browser.close();
  console.log('=== DONE ===');
  process.exit(ok ? 0 : 1);
})().catch(e => { console.error('FATAL:', e); process.exit(1); });
