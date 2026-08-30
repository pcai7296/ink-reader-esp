// browser_upload_test.js — 用真实 Chrome 模拟手机浏览器上传设备文件（复现手机上传问题）
// 用法: node browser_upload_test.js
const { chromium } = require('playwright-core');

const DEV = 'http://192.168.4.1';
const TEST_FILE = 'C:\\Users\\Administrator\\AppData\\Local\\Temp\\pw_upload_test.txt';

(async () => {
  // 模拟手机（移动端视口 + Android UA）——最接近手机浏览器的真实行为
  const browser = await chromium.launch({
    executablePath: 'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe',
    headless: false,   // 有头：真实浏览器观察（最接近手机）
    args: ['--no-first-run', '--disable-infobars', '--window-size=390,844']
  });
  const context = await browser.newContext({
    viewport: { width: 390, height: 844 },
    isMobile: true,
    hasTouch: true,
    userAgent: 'Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Mobile Safari/537.36'
  });
  const page = await context.newPage();

  console.log('=== 1. 打开管理页 /fs/edit ===');
  try {
    const resp = await page.goto(DEV + '/fs/edit', { waitUntil: 'domcontentloaded', timeout: 15000 });
    console.log('page status:', resp ? resp.status() : 'none', 'title:', await page.title());
  } catch (e) { console.log('goto FAIL:', e.message); }

  console.log('=== 2. 截图管理页 ===');
  await page.screenshot({ path: 'mgr_page.png', fullPage: false });

  // 找上传 input（fileSelector）和上传按钮/路径输入
  console.log('=== 3. 找上传要素 ===');
  const inputs = await page.$$('input');
  console.log('inputs found:', inputs.length);
  for (const inp of inputs) {
    const type = await inp.getAttribute('type');
    const id = await inp.getAttribute('id');
    const cls = await inp.getAttribute('class');
    console.log('  input type=' + type + ' id=' + id + ' class=' + cls);
  }
  // 文件选择 input (type=file)
  const fileInput = await page.$('input[type=file]');
  if (fileInput) {
    console.log('  fileInput OK');
    // 写入测试文件
    require('fs').writeFileSync(TEST_FILE, 'playwright real-browser upload ' + Date.now());
    console.log('  set input files: ' + TEST_FILE);
    await fileInput.setInputFiles(TEST_FILE);
  } else {
    console.log('  NO file input found!');
  }

  console.log('=== 4. 截图文件选择后 ===');
  await page.screenshot({ path: 'mgr_file_selected.png' });

  await browser.close();
  console.log('=== DONE (只测到选择文件, 上传按钮交互待看截图) ===');
})().catch(e => { console.error('FATAL:', e); process.exit(1); });
