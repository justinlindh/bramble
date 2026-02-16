// Config page — identity, radio settings, channels

let app;
let config = null;

function render() {
    const el = document.getElementById('page-config');
    if (!config) {
        el.innerHTML = `<div class="empty">${app.connected ? 'Loading…' : 'Connect to view config'}</div>`;
        return;
    }
    const r = config.radio ?? {};
    const channels = config.channels ?? [];

    el.innerHTML = `
        <div class="config-section">
            <h3>Identity</h3>
            <div class="config-row"><label>Address</label><span class="mono">${formatAddr(config.address)}</span></div>
            <div class="config-row"><label>Public Key</label><span class="mono">${truncHash(config.pubkey)}</span></div>
        </div>

        <div class="config-section">
            <h3>Radio</h3>
            <div class="config-row"><label>TX Power</label><input id="cfg-txpow" type="number" value="${r.tx_power ?? 20}" min="2" max="20"> dBm</div>
            <div class="config-row"><label>Spreading Factor</label>
                <select id="cfg-sf">${[7,8,9,10,11,12].map(s => `<option ${s === (r.sf ?? 10) ? 'selected' : ''}>${s}</option>`).join('')}</select>
            </div>
            <div class="config-row"><label>Bandwidth</label>
                <select id="cfg-bw">${[125,250,500].map(b => `<option ${b === (r.bw ?? 125) ? 'selected' : ''}>${b}</option>`).join('')}</select> kHz
            </div>
            <div class="config-row"><label>Coding Rate</label>
                <select id="cfg-cr">${[5,6,7,8].map(c => `<option ${c === (r.cr ?? 5) ? 'selected' : ''}>4/${c}</option>`).join('')}</select>
            </div>
            <button class="btn btn-sm primary" id="cfg-save-radio">Save Radio</button>
        </div>

        <div class="config-section">
            <h3>Channels</h3>
            <div class="channel-list">
                ${channels.map((ch, i) => `
                    <div class="channel-item">
                        <span>${escHtml(ch.name)}${ch.psk ? ' 🔒' : ''}</span>
                        <button data-ch="${i}">✕</button>
                    </div>`).join('')}
                ${channels.length === 0 ? '<div class="empty" style="padding:0.5rem">No channels</div>' : ''}
            </div>
            <div style="display:flex;gap:0.5rem;margin-top:0.5rem">
                <input id="cfg-ch-name" placeholder="Name" style="flex:1;padding:0.3rem 0.5rem;background:var(--surface);color:var(--text);border:1px solid var(--border);border-radius:4px;font-size:0.85rem">
                <input id="cfg-ch-psk" placeholder="PSK (optional)" style="flex:1;padding:0.3rem 0.5rem;background:var(--surface);color:var(--text);border:1px solid var(--border);border-radius:4px;font-size:0.85rem">
                <button class="btn btn-sm success" id="cfg-add-ch">Add</button>
            </div>
        </div>`;

    document.getElementById('cfg-save-radio').addEventListener('click', saveRadio);
    document.getElementById('cfg-add-ch').addEventListener('click', addChannel);
    el.querySelectorAll('.channel-item button').forEach(btn => {
        btn.addEventListener('click', () => removeChannel(parseInt(btn.dataset.ch)));
    });
}

async function saveRadio() {
    try {
        await app.rpc('set_config', {
            radio: {
                tx_power: parseInt(document.getElementById('cfg-txpow').value),
                sf: parseInt(document.getElementById('cfg-sf').value),
                bw: parseInt(document.getElementById('cfg-bw').value),
                cr: parseInt(document.getElementById('cfg-cr').selectedOptions[0].text.split('/')[1]),
            }
        });
        await refresh();
    } catch (e) { alert('Save failed: ' + e.message); }
}

async function addChannel() {
    const name = document.getElementById('cfg-ch-name').value.trim();
    if (!name) return;
    const psk = document.getElementById('cfg-ch-psk').value.trim() || undefined;
    try {
        await app.rpc('set_config', { add_channel: { name, psk } });
        await refresh();
    } catch (e) { alert('Failed: ' + e.message); }
}

async function removeChannel(index) {
    try {
        await app.rpc('set_config', { remove_channel: index });
        await refresh();
    } catch (e) { alert('Failed: ' + e.message); }
}

function formatAddr(a) {
    if (a == null) return '—';
    return typeof a === 'number' ? '0x' + a.toString(16).toUpperCase().padStart(4, '0') : String(a);
}

function truncHash(h) {
    if (!h) return '—';
    const s = String(h);
    return s.length > 16 ? s.slice(0, 8) + '…' + s.slice(-8) : s;
}

function escHtml(s) {
    return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

async function refresh() {
    if (!app.connected) return;
    try {
        config = await app.rpc('get_config');
    } catch { config = null; }
    render();
}

export function initConfig(appRef) {
    app = appRef;
    render();
    document.addEventListener('bramble-connected', () => refresh());
    document.addEventListener('bramble-page', (e) => {
        if (e.detail.page === 'config') refresh();
    });
}
