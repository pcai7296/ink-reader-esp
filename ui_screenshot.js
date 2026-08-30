// ui_screenshot.js — 对 /fs/edit 新版 UI 截图 (桌面 + 移动视口), 验证美化效果
const { chromium } = require('playwright-core');
const DEV = 'http://' + (process.argv[2] || '192.168.4.1');

(async () => {
  const browser = await chromium.launch({
    executablePath: 'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe',
    headless: true,
    args: ['--no-first-run', '--disable-infobars', '--disable-gpu']
  });

  // 桌面视口
  const ctxD = await browser.newContext({ viewport: { width: 1100, height: 720 } });
  const pageD = await ctxD.newPage();
  await pageD.goto(DEV + '/fs/edit', { waitUntil: 'domcontentloaded', timeout: 20000 });
  await pageD.waitForTimeout(2500);   // 等 status + 根目录 list 加载
  await pageD.screenshot({ path: 'ui_desktop.png', fullPage: false });
  console.log('desktop saved, tree children:', await pageD.evaluate(() => {
    const t = document.getElementById('tree');
    return t ? (t.querySelectorAll('li').length) : -1;
  }));
  await ctxD.close();

  // 移动视口 (手机)
  const ctxM = await browser.newContext({
    viewport: { width: 390, height: 844 },
    isMobile: true, hasTouch: true,
    userAgent: 'Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Mobile Safari/537.36'
  });
  const pageM = await ctxM.newPage();
  await pageM.goto(DEV + '/fs/edit', { waitUntil: 'domcontentloaded', timeout: 20000 });
  await pageM.waitForTimeout(2500);
  await pageM.screenshot({ path: 'ui_mobile.png', fullPage: false });
  console.log('mobile saved, tree children:', await pageM.evaluate(() => {
    const t = document.getElementById('tree');
    return t ? (t.querySelectorAll('li').length) : -1;
  }));
  await ctxM.close();

  await browser.close();
  console.log('=== DONE ===');
})().catch(e => { console.error('FATAL:', e); process.exit(1); });
