// Bramble Web App — main controller

import { SerialTransport } from './serial.js';
import { BLETransport } from './ble.js';
import { initChat } from './pages/chat.js';
import { initNodes } from './pages/nodes.js';
import { initConfig } from './pages/config.js';
import { initStats } from './pages/stats.js';

let transport = null;

function getTransport(type) {
    return type === 'ble' ? new BLETransport() : new SerialTransport();
}

// Expose a shared API for pages
export const app = {
    get transport() { return transport; },
    get connected() { return transport?.connected ?? false; },
    async rpc(method, params = {}) {
        if (!transport?.connected) throw new Error('Not connected');
        return transport.sendRPC(method, params);
    },
};

function setStatus(connected) {
    const dot = document.getElementById('status-dot');
    const btn = document.getElementById('connect-btn');
    dot.className = connected ? 'dot connected' : 'dot disconnected';
    btn.textContent = connected ? 'Disconnect' : 'Connect';
}

async function handleConnect() {
    if (transport?.connected) {
        await transport.disconnect();
        setStatus(false);
        return;
    }
    const type = document.getElementById('transport-select').value;
    transport = getTransport(type);

    transport.onNotification((method, params) => {
        document.dispatchEvent(new CustomEvent('bramble-notify', { detail: { method, params } }));
    });

    try {
        await transport.connect();
        setStatus(true);
        // Refresh active page
        document.dispatchEvent(new CustomEvent('bramble-connected'));
    } catch (err) {
        setStatus(false);
        alert('Connection failed: ' + err.message);
    }
}

// Tab switching
function initTabs() {
    const tabs = document.querySelectorAll('.tab');
    const pages = document.querySelectorAll('.page');
    tabs.forEach(tab => {
        tab.addEventListener('click', () => {
            const target = tab.dataset.page;
            tabs.forEach(t => t.classList.toggle('active', t === tab));
            pages.forEach(p => p.classList.toggle('active', p.id === `page-${target}`));
            document.dispatchEvent(new CustomEvent('bramble-page', { detail: { page: target } }));
        });
    });
}

document.addEventListener('DOMContentLoaded', () => {
    initTabs();
    document.getElementById('connect-btn').addEventListener('click', handleConnect);

    // Init all pages
    initChat(app);
    initNodes(app);
    initConfig(app);
    initStats(app);
});
