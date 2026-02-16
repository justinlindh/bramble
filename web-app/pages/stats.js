// Stats page — airtime, counters, uptime

let app;
let status = null;

function render() {
    const el = document.getElementById('page-stats');
    if (!status) {
        el.innerHTML = `<div class="empty">${app.connected ? 'Loading…' : 'Connect to view stats'}</div>`;
        return;
    }
    const airtime = status.airtime ?? {};
    const tiers = [
        { label: 'Critical', key: 'critical', cls: 'critical' },
        { label: 'Normal', key: 'normal', cls: 'normal' },
        { label: 'Bulk', key: 'bulk', cls: 'bulk' },
    ];

    el.innerHTML = `
        <div class="stat-grid">
            <div class="stat-card"><div class="stat-value">${status.tx_count ?? 0}</div><div class="stat-label">TX Packets</div></div>
            <div class="stat-card"><div class="stat-value">${status.rx_count ?? 0}</div><div class="stat-label">RX Packets</div></div>
            <div class="stat-card"><div class="stat-value">${status.neighbors ?? 0}</div><div class="stat-label">Neighbors</div></div>
            <div class="stat-card"><div class="stat-value">${status.routes ?? 0}</div><div class="stat-label">Routes</div></div>
            <div class="stat-card"><div class="stat-value">${formatUptime(status.uptime)}</div><div class="stat-label">Uptime</div></div>
            <div class="stat-card"><div class="stat-value">${formatMem(status.free_heap)}</div><div class="stat-label">Free Heap</div></div>
        </div>

        <div class="airtime-bar">
            <h3>Airtime Budget</h3>
            ${tiers.map(t => {
                const pct = airtime[t.key] ?? 0;
                return `<div class="tier">
                    <span class="tier-label">${t.label}</span>
                    <div class="tier-track"><div class="tier-fill ${t.cls}" style="width:${Math.min(pct, 100)}%"></div></div>
                    <span class="tier-pct">${pct.toFixed(1)}%</span>
                </div>`;
            }).join('')}
        </div>`;
}

function formatUptime(s) {
    if (s == null) return '—';
    if (s < 60) return s + 's';
    if (s < 3600) return Math.floor(s / 60) + 'm';
    if (s < 86400) return Math.floor(s / 3600) + 'h';
    return Math.floor(s / 86400) + 'd';
}

function formatMem(bytes) {
    if (bytes == null) return '—';
    if (bytes > 1024) return (bytes / 1024).toFixed(0) + 'K';
    return bytes + 'B';
}

async function refresh() {
    if (!app.connected) return;
    try {
        status = await app.rpc('get_status');
    } catch { status = null; }
    render();
}

export function initStats(appRef) {
    app = appRef;
    render();
    document.addEventListener('bramble-connected', () => refresh());
    document.addEventListener('bramble-page', (e) => {
        if (e.detail.page === 'stats') refresh();
    });
}
