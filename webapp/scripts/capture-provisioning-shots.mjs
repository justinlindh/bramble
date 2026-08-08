/**
 * Capture the provisioning walkthrough screenshots against the mock node.
 *
 * Two passes, each against a freshly restarted server so the node really is
 * unprovisioned at the start:
 *   PASS=found  ... connect, show the inert banner, found a network, print the key
 *   PASS=join KEY=<hex> ... connect a second node and join it to that key
 */
import { chromium } from 'playwright';
import { mkdirSync } from 'node:fs';

const APP = process.env.APP || 'http://localhost:8185/';
const OUT = process.env.OUT || '/tmp/shots';
const NODE = process.env.NODE_ADDR || 'localhost:8185';
const PASS = process.env.PASS || 'found';
const KEY = process.env.KEY || '';
mkdirSync(OUT, { recursive: true });

const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 1280, height: 860 }, deviceScaleFactor: 2 });
page.on('pageerror', (e) => console.log('[pageerror]', String(e).slice(0, 300)));

const shot = async (name) => {
  await page.waitForTimeout(500);
  await page.screenshot({ path: `${OUT}/${name}.png` });
  console.log('captured', name);
};

// Screenshot just the Network Key card, so a doc image is the section itself
// rather than whatever else happened to be scrolled into view around it.
const shotNetworkKeyCard = async (name) => {
  await page.waitForTimeout(500);
  const card = page
    .locator('section')
    .filter({ has: page.getByRole('heading', { name: 'Network Key', exact: true }) })
    .first();
  await card.screenshot({ path: `${OUT}/${name}.png` });
  console.log('captured', name, '(card)');
};

// Scroll the Network Key section to the top of the viewport and frame it.
const gotoNetworkKey = async () => {
  await page.getByRole('button', { name: /^Config$/ }).first().click();
  await page.waitForTimeout(800);
  const heading = page.getByRole('heading', { name: 'Network Key', exact: true }).first();
  await heading.scrollIntoViewIfNeeded();
  // Nudge the section clear of the sticky header so the whole card is framed.
  await page.mouse.wheel(0, -60);
  await page.waitForTimeout(700);
};

await page.goto(APP, { waitUntil: 'networkidle' });
await page.waitForTimeout(900);

if (PASS === 'found') await shot('01-connect');

// Connect to the local mock node over the WiFi transport (the "Mock Node"
// button targets a fixed dev port, not this server).
await page.getByRole('button', { name: /^WiFi$/ }).click();
await page.waitForTimeout(400);
await page.locator('input[type="text"]').first().fill(NODE);
await page.getByRole('button', { name: /^Connect$/ }).click();

// Wait for the status poll to land so the inert banner is actually rendered.
await page.getByText(/UNPROVISIONED and inert/i).waitFor({ timeout: 15000 });

if (PASS === 'found') {
  await shot('02-unprovisioned-banner');

  await gotoNetworkKey();
  await shotNetworkKeyCard('03-network-key-unprovisioned');

  await page.getByRole('button', { name: /^Generate key$/ }).click();
  await page.getByText(/Fingerprint/i).first().waitFor({ timeout: 15000 });
  await page.waitForTimeout(800);
  await shotNetworkKeyCard('04-founded-network');

  const key = await page.getByLabel('Generated network key hex').inputValue();
  console.log('FOUNDER_KEY=' + key);
} else {
  await gotoNetworkKey();
  await shotNetworkKeyCard('05-join-paste');

  await page.getByLabel('Network key share string').fill(KEY);
  // "Provision" also names the banner's jump-to-config button; take the one in
  // the join form.
  await page.locator('form').getByRole('button', { name: /^Provision$/ }).click();
  await page.getByText(/joined the network/i).waitFor({ timeout: 15000 });
  await page.waitForTimeout(900);
  await shotNetworkKeyCard('06-joined-converged');
}

await browser.close();
console.log('done');
