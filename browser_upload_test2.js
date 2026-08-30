// browser_upload_test2.js — 真实 Chrome 模拟手机完整上传流程（选文件→点上传→等响应）
const { chromium } = require('playwright-core');

const DEV = 'http://192.168.4.1';
const TEST_FILE = 'C:\\Users\\Administrator\\AppData\\Local\\Temp\\pw_upload2.txt';

(async () => {
  const browser = await chromium.launch({
    executablePath: 'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe',
    headless: false,
    args: ['--no-first-run', '--disable-infobars', '--window-size=390,844']
  });
  const context = await browser.newContext({
    viewport: { width: 390, height: 844 }, isMobile: true, hasTouch: true,
    userAgent: 'Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Mobile Safari/537.36'
  });
  const page = await context.newPage();

  // 捕获所有控制台错误（浏览器行为线索）
  page.on('console', m => { if (m.type() === 'error') console.log('CONSOLE-ERR:', m.text()); });
  page.on('requestfailed', r => console.log('REQ-FAIL:', r.url(), r.failure() && r.failure().errorText));
  // 捕获上传响应
  let uploadResp = null;
  page.on('response', r => { if (r.url().includes('/fs/edit') && r.request().method() === 'POST') { console.log('POST-STATUS:', r.status()); r.text().then(t => console.log('POST-BODY:', t)).catch(()=>{}); } });

  console.log('=== 打开管理页 ===');
  await page.goto(DEV + '/fs/edit', { waitUntil: 'domcontentloaded', timeout: 15000 });

  // 选文件
  require('fs').writeFileSync(TEST_FILE, 'playwright real upload full-flow ' + Date.now());
  const fileInput = await page.$('input[type=file]');
  if (!fileInput) { console.log('NO file input'); await browser.close(); return; }
  await fileInput.setInputFiles(TEST_FILE);
  console.log('file selected:', TEST_FILE);

  // 确认 pathInput 是根 "/"
  const pathInput = await page.$('#pathInput');
  if (pathInput) { const v = await pathInput.inputValue(); console.log('pathInput value:', JSON.stringify(v)); }

  // 找"上传"按钮并点击。manager.htm: upload.onclick -> tree.httpUpload(file, pathInput.value)
  const uploadBtn = await page.$('text=上传') || (await page.$('button:has-text("上传")'));
  if (uploadBtn) {
    console.log('clicking 上传...');
    await uploadBtn.click();
    // 等上传完成（httpUpload 用 setLoading 显示"上传中"→onload 回调）
    await page.waitForTimeout(3000);
    console.log('after click wait 3s');
  } else {
    console.log('NO upload button (text=上传)');
  }

  await page.screenshot({ path: 'upload_result.png', fullPage: false });
  // 刷新列表看是否显示新文件
  await page.waitForTimeout(1000);
  await page.reload({ waitUntil: 'domcontentloaded' });
  await page.waitForTimeout(1500);
  await page.screenshot({ path: 'after_upload_refresh.png', fullPage: false });
  console.log('=== DONE ===');
  await browser.close();
})().catch(e => { console.error('FATAL:', e); process.exit(1); });
