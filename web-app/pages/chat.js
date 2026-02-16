// Chat page — DM and channel messaging

let app;
let currentDest = 'broadcast';
let messages = [];

function render() {
    const el = document.getElementById('page-chat');
    el.innerHTML = `
        <div class="chat-tabs" id="chat-dest-tabs">
            <button class="chat-tab ${currentDest === 'broadcast' ? 'active' : ''}" data-dest="broadcast">📢 Broadcast</button>
        </div>
        <div class="message-list" id="message-list">
            ${messages.length === 0
                ? '<div class="empty">No messages yet</div>'
                : messages.map(m => `
                    <div class="msg ${m.from === 'self' ? 'outgoing' : 'incoming'}">
                        <div>${escHtml(m.text)}</div>
                        <div class="msg-meta">
                            <span>${m.from !== 'self' ? formatAddr(m.from) + ' · ' : ''}${formatTime(m.ts)}</span>
                            <span class="msg-status">${m.from === 'self' ? statusIcon(m.status) : ''}</span>
                        </div>
                    </div>`).join('')}
        </div>
        <div class="compose">
            <input id="chat-input" type="text" placeholder="${app.connected ? 'Type a message…' : 'Connect to send'}" ${app.connected ? '' : 'disabled'}>
            <button class="btn btn-sm primary" id="chat-send" ${app.connected ? '' : 'disabled'}>Send</button>
        </div>`;

    document.getElementById('chat-send').addEventListener('click', sendMessage);
    document.getElementById('chat-input').addEventListener('keydown', e => {
        if (e.key === 'Enter') sendMessage();
    });

    // Scroll to bottom
    const list = document.getElementById('message-list');
    list.scrollTop = list.scrollHeight;
}

async function sendMessage() {
    const input = document.getElementById('chat-input');
    const text = input.value.trim();
    if (!text || !app.connected) return;

    const msg = { from: 'self', text, ts: Date.now(), status: 'sending' };
    messages.push(msg);
    input.value = '';
    render();

    try {
        await app.rpc('send_message', { dest: currentDest, text });
        msg.status = 'delivered';
    } catch {
        msg.status = 'failed';
    }
    render();
}

async function refresh() {
    if (!app.connected) return;
    try {
        const result = await app.rpc('get_messages');
        if (Array.isArray(result?.messages)) {
            // Merge incoming messages
            for (const m of result.messages) {
                if (!messages.find(x => x.id === m.id)) {
                    messages.push({ id: m.id, from: m.from ?? 'unknown', text: m.text, ts: m.ts ?? Date.now(), status: 'confirmed' });
                }
            }
        }
    } catch { /* ignore */ }
    render();
}

function statusIcon(s) {
    if (s === 'sending') return '●';
    if (s === 'delivered') return '✓';
    if (s === 'confirmed') return '✓✓';
    if (s === 'failed') return '✗';
    return '';
}

function formatAddr(a) {
    if (!a) return '??';
    return typeof a === 'number' ? '0x' + a.toString(16).toUpperCase().padStart(4, '0') : String(a);
}

function formatTime(ts) {
    if (!ts) return '';
    const d = new Date(typeof ts === 'number' && ts < 1e12 ? ts * 1000 : ts);
    return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
}

function escHtml(s) {
    return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

export function initChat(appRef) {
    app = appRef;
    render();

    document.addEventListener('bramble-connected', () => refresh());
    document.addEventListener('bramble-page', (e) => {
        if (e.detail.page === 'chat') refresh();
    });
    document.addEventListener('bramble-notify', (e) => {
        if (e.detail.method === 'new_message') {
            const m = e.detail.params;
            messages.push({ id: m.id, from: m.from, text: m.text, ts: m.ts ?? Date.now(), status: 'confirmed' });
            render();
        }
        if (e.detail.method === 'msg_ack') {
            const acked = messages.find(x => x.id === e.detail.params?.id);
            if (acked) { acked.status = 'confirmed'; render(); }
        }
    });
}
