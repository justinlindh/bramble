import type { SimNode, NodeStats } from '../types';

interface NodeHealthCardProps {
  node: SimNode | null;
  stats: NodeStats | null;
  neighborCount: number;
  onClose: () => void;
}

export function NodeHealthCard({ node, stats, neighborCount, onClose }: NodeHealthCardProps) {
  if (!node) return null;

  const s = stats ?? {
    packetsSent: 0, packetsReceived: 0, packetsForwarded: 0,
    routeCount: 0, messagesOriginated: 0, messagesDelivered: 0,
  };

  return (
    <div style={{
      background: '#161b22',
      border: '1px solid #30363d',
      borderRadius: '8px',
      padding: '14px 16px',
      fontFamily: "'Fira Code', 'Consolas', monospace",
      fontSize: '12px',
      color: '#e6edf3',
    }}>
      {/* Header */}
      <div style={{
        display: 'flex',
        justifyContent: 'space-between',
        alignItems: 'center',
        marginBottom: '12px',
        paddingBottom: '8px',
        borderBottom: '1px solid #21262d',
      }}>
        <div>
          <div style={{ fontSize: '14px', fontWeight: 700 }}>
            Node {node.id}
          </div>
          {node.addr && (
            <div style={{ fontSize: '11px', color: '#8b949e', marginTop: '2px' }}>
              {node.addr}
            </div>
          )}
        </div>
        <button
          onClick={onClose}
          style={{
            background: 'none',
            border: 'none',
            color: '#8b949e',
            cursor: 'pointer',
            fontSize: '16px',
            padding: '0 4px',
          }}
        >
          ✕
        </button>
      </div>

      {/* Position */}
      <Row label="Position" value={`(${Math.round(node.x)}, ${Math.round(node.y)})`} />
      <Row label="Status" value={node.active ? '● Active' : '○ Inactive'} color={node.active ? '#3fb950' : '#6e7681'} />

      {/* Divider */}
      <div style={{ borderTop: '1px solid #21262d', margin: '8px 0' }} />

      {/* Packet Stats */}
      <div style={{ fontSize: '10px', color: '#8b949e', textTransform: 'uppercase', letterSpacing: '0.08em', marginBottom: '6px' }}>
        Packets
      </div>
      <Row label="Sent" value={String(s.packetsSent)} color="#f0883e" />
      <Row label="Received" value={String(s.packetsReceived)} color="#58a6ff" />
      <Row label="Forwarded" value={String(s.packetsForwarded)} color="#d2a8ff" />

      {/* Divider */}
      <div style={{ borderTop: '1px solid #21262d', margin: '8px 0' }} />

      {/* Network */}
      <div style={{ fontSize: '10px', color: '#8b949e', textTransform: 'uppercase', letterSpacing: '0.08em', marginBottom: '6px' }}>
        Network
      </div>
      <Row label="Routes" value={String(s.routeCount)} color="#58a6ff" />
      <Row label="Neighbors" value={String(neighborCount)} color="#3fb950" />

      {/* Divider */}
      <div style={{ borderTop: '1px solid #21262d', margin: '8px 0' }} />

      {/* Messages */}
      <div style={{ fontSize: '10px', color: '#8b949e', textTransform: 'uppercase', letterSpacing: '0.08em', marginBottom: '6px' }}>
        Messages
      </div>
      <Row label="Originated" value={String(s.messagesOriginated)} color="#f0883e" />
      <Row label="Delivered" value={String(s.messagesDelivered)} color="#3fb950" />
    </div>
  );
}

function Row({ label, value, color }: { label: string; value: string; color?: string }) {
  return (
    <div style={{
      display: 'flex',
      justifyContent: 'space-between',
      padding: '2px 0',
    }}>
      <span style={{ color: '#8b949e' }}>{label}</span>
      <span style={{ color: color ?? '#e6edf3', fontWeight: 600 }}>{value}</span>
    </div>
  );
}
