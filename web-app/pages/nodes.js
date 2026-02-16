// Nodes page — peer list with route details

let app;
let nodes = [];

function render() {
    const el = document.getElementById('page-nodes');
    if (!nodes.length) {
        el.innerHTML = `<div class="empty">${app.connected ? 'No nodes discovered yet' : 'Connect to view nodes'}</div>`;
        return;
    }
    el.innerHTML = `
        <div class="node-list">
            ${nodes.map((n, i) => `
                <div class="node-card" data-idx="${i}">
                    <div class="node-header">
                        <span class="node-addr">${formatAddr(n.address)}</span>
                        <span class="node-rssi">${n.rssi != null ? n.rssi + ' dBm' : '—'}</span>
                    </div>
                    <div class="node-detail">
                        Last heard: ${formatAgo(n.last_heard)} · ${n.route_state ?? 'unknown'}
                    </div>
                    <div class="node-route">
                        Hops: ${n.hops ?? '?'} · Metric: ${n.metric ?? '?'} · Next hop: ${formatAddr(n.next_hop)}
                    </div>
                </div>`).join('')}
        </div>`;

    el.querySelectorAll('.node-card').forEach(card => {
        card.addEventListener('click', () => card.classList.toggle('expanded'));
    });
}

function formatAddr(a) {
    if (a == null) return '—';
    return typeof a === 'number' ? '0x' + a.toString(16).toUpperCase().padStart(4, '0') : String(a);
}

function formatAgo(ts) {
    if (!ts) return 'never';
    const sec = Math.floor((Date.now() - (typeof ts === 'number' && ts < 1e12 ? ts * 1000 : ts)) / 1000);
    if (sec < 60) return sec + 's ago';
    if (sec < 3600) return Math.floor(sec / 60) + 'm ago';
    return Math.floor(sec / 3600) + 'h ago';
}

async function refresh() {
    if (!app.connected) return;
    try {
        const result = await app.rpc('get_nodes');
        nodes = result?.nodes ?? [];
        // Also fetch routes and merge
        try {
            const routes = await app.rpc('get_routes');
            if (Array.isArray(routes?.routes)) {
                for (const r of routes.routes) {
                    const node = nodes.find(n => n.address === r.dest);
                    if (node) Object.assign(node, { hops: r.hops, metric: r.metric, next_hop: r.next_hop, route_state: r.state });
                }
            }
        } catch { /* optional */ }
    } catch { nodes = []; }
    render();
}

export function initNodes(appRef) {
    app = appRef;
    render();
    document.addEventListener('bramble-connected', () => refresh());
    document.addEventListener('bramble-page', (e) => {
        if (e.detail.page === 'nodes') refresh();
    });
}
