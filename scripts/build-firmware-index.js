#!/usr/bin/env node
const fs = require('fs');
const path = require('path');

const otaRoot = process.argv[2] || process.env.OTA_ROOT || '/home/user/src/dockers/ota';
const outFile = process.argv[3] || path.join(otaRoot, 'index.json');

function exists(p){ try { fs.accessSync(p); return true; } catch { return false; } }
if (!exists(otaRoot)) throw new Error(`OTA root not found: ${otaRoot}`);

const channels = fs.readdirSync(otaRoot, { withFileTypes: true })
  .filter(d => d.isDirectory())
  .map(d => d.name)
  .filter(n => !n.startsWith('.'));

const releases = [];
for (const channel of channels) {
  const cdir = path.join(otaRoot, channel);
  const versions = fs.readdirSync(cdir, { withFileTypes: true }).filter(d => d.isDirectory()).map(d => d.name);
  for (const version of versions) {
    const vdir = path.join(cdir, version);
    const metaFile = path.join(vdir, 'release-meta.json');
    const publishedAt = exists(metaFile)
      ? JSON.parse(fs.readFileSync(metaFile, 'utf8')).published_at
      : new Date(fs.statSync(vdir).mtimeMs).toISOString();

    const artifacts = [];
    const boards = fs.readdirSync(vdir, { withFileTypes: true }).filter(d => d.isDirectory()).map(d => d.name);
    for (const board of boards) {
      const bdir = path.join(vdir, board);
      for (const file of fs.readdirSync(bdir)) {
        const fpath = path.join(bdir, file);
        if (!fs.statSync(fpath).isFile()) continue;
        if (file.endsWith('.meta.json')) continue;
        const metaPath = `${fpath}.meta.json`;
        let sha256 = '';
        let size = fs.statSync(fpath).size;
        let meta = {};
        if (exists(metaPath)) {
          meta = JSON.parse(fs.readFileSync(metaPath, 'utf8'));
          sha256 = meta.sha256 || '';
          size = meta.size || size;
        }
        artifacts.push({
          board,
          file: `/ota/${channel}/${version}/${board}/${file}`,
          sha256,
          size,
          ...(meta.notes ? { notes: String(meta.notes) } : {}),
        });
      }
    }
    releases.push({ version, published_at: publishedAt, channel, artifacts });
  }
}

releases.sort((a,b)=> new Date(b.published_at)-new Date(a.published_at) || b.version.localeCompare(a.version, undefined, {numeric:true, sensitivity:'base'}));
const out = { releases };
fs.writeFileSync(outFile, JSON.stringify(out, null, 2));
console.log(`Wrote ${outFile} with ${releases.length} release(s)`);
