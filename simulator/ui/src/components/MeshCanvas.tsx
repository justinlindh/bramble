import React, { useMemo, useEffect, useRef, useState, useCallback } from 'react';
import type { SimNode, SimEvent, PacketAnimation, DeliveryPathAnimation, LinkActivity, BrokenLink, LinkQuality } from '../types';

interface MeshCanvasProps {
  nodes: Map<string, SimNode>;
  radioRange?: number;
  events?: SimEvent[];
  ws?: WebSocket | null;
  deliveryPaths?: DeliveryPathAnimation[];
  linkActivity?: Map<string, LinkActivity>;
  brokenLinks?: Map<string, BrokenLink>;
  linkQuality?: Map<string, LinkQuality>;
  selectedNodeId?: string | null;
  onNodeClick?: (nodeId: string) => void;
}

const PADDING = 60;
const NODE_RADIUS = 18;
const GRID_SIZE = 50;
const RECENT_TRAFFIC_MS = 5000; // links active in last 5s are "recent"
const BROKEN_LINK_FADE_MS = 10000;

// RSSI color scale for link visualization
// Strong (> -70 dBm): bright green
// Good (-70 to -85 dBm): yellow-green
// Fair (-85 to -100 dBm): orange
// Weak (< -100 dBm): red
function rssiToColor(rssi: number): string {
  if (rssi > -70) return '#00ff88';      // bright green: strong
  if (rssi > -85) return '#c8e838';      // yellow-green: good
  if (rssi > -100) return '#f0883e';     // orange: fair
  return '#f85149';                       // red: weak
}

// Line width: thicker = stronger signal. Range 1.0–4.5
function rssiToWidth(rssi: number): number {
  // Map [-110, -40] → [1.0, 4.5]
  const clamped = Math.max(-110, Math.min(-40, rssi));
  const norm = (clamped - (-110)) / ((-40) - (-110)); // 0 to 1
  return 1.0 + norm * 3.5;
}

// Packet type colors + glow
const PKT_COLORS: Record<string, { fill: string; glow: string }> = {
  RREQ:   { fill: '#58a6ff', glow: '#1f6feb' },
  RREP:   { fill: '#3fb950', glow: '#238636' },
  DATA:   { fill: '#f0883e', glow: '#bd561d' },
  RERR:   { fill: '#f85149', glow: '#b91c1c' },
  BEACON: { fill: '#8b949e', glow: '#484f58' },
};

function pktColor(pkt_type: string) {
  return PKT_COLORS[pkt_type] ?? PKT_COLORS.DATA;
}

function makeLinkKey(a: string, b: string): string {
  return a < b ? `${a}-${b}` : `${b}-${a}`;
}

function computeViewBox(nodes: SimNode[], width: number, height: number) {
  if (nodes.length === 0) {
    return { scaleX: 1, scaleY: 1, offsetX: width / 2, offsetY: height / 2 };
  }

  const xs = nodes.map(n => n.x);
  const ys = nodes.map(n => n.y);
  const minX = Math.min(...xs);
  const maxX = Math.max(...xs);
  const minY = Math.min(...ys);
  const maxY = Math.max(...ys);

  const dataW = maxX - minX || 1;
  const dataH = maxY - minY || 1;
  const drawW = width - PADDING * 2;
  const drawH = height - PADDING * 2;

  const scale = Math.min(drawW / dataW, drawH / dataH, 4);

  const offsetX = PADDING + (drawW - dataW * scale) / 2 - minX * scale;
  const offsetY = PADDING + (drawH - dataH * scale) / 2 - minY * scale;

  return { scale, offsetX, offsetY };
}

function toScreen(x: number, y: number, transform: { scale?: number; scaleX?: number; scaleY?: number; offsetX: number; offsetY: number }) {
  const sx = 'scale' in transform && transform.scale !== undefined ? transform.scale : (transform.scaleX ?? 1);
  const sy = 'scale' in transform && transform.scale !== undefined ? transform.scale : (transform.scaleY ?? 1);
  return { sx: x * sx + transform.offsetX, sy: y * sy + transform.offsetY };
}

// Resolve a hex address like "0x01000008" to a node
function resolveAddrToNode(addr: string, nodes: Map<string, SimNode>): SimNode | null {
  if (nodes.has(addr)) return nodes.get(addr)!;
  for (const node of nodes.values()) {
    if (node.addr === addr) return node;
  }
  return null;
}

export function MeshCanvas({
  nodes, radioRange = 150, events = [], ws,
  deliveryPaths = [], linkActivity, brokenLinks, linkQuality,
  selectedNodeId, onNodeClick,
}: MeshCanvasProps) {
  const [showRssiLabels, setShowRssiLabels] = useState(false);
  const nodeList = useMemo(() => Array.from(nodes.values()), [nodes]);
  const svgRef = useRef<SVGSVGElement>(null);
  const dragRef = useRef<{ nodeId: string; startX: number; startY: number; origX: number; origY: number } | null>(null);
  const [dragPos, setDragPos] = useState<{ nodeId: string; x: number; y: number } | null>(null);
  const [overTrash, setOverTrash] = useState(false);
  const [contextMenu, setContextMenu] = useState<{ nodeId: string; x: number; y: number } | null>(null);
  const dragStartTime = useRef(0);

  // Build recent packet animations from events
  const [recentPackets, setRecentPackets] = useState<PacketAnimation[]>([]);
  const packetCounterRef = useRef(0);

  useEffect(() => {
    const now = Date.now();
    setRecentPackets(prev => {
      const alive = prev.filter(p => now - p.createdAt < p.durationMs);
      return alive;
    });
  }, [events]);

  const lastEventId = useRef(0);
  useEffect(() => {
    const newEvents = events.filter(e => e.type === 'packet_sent' && e.id > lastEventId.current);
    if (newEvents.length === 0) return;
    lastEventId.current = Math.max(...newEvents.map(e => e.id));

    const now = Date.now();
    const newAnims: PacketAnimation[] = newEvents.map(e => {
      packetCounterRef.current++;
      const fromNode = (e.details.node ?? e.details.from) as string | undefined;
      const toAddr   = (e.details.dest ?? e.details.dest_addr) as string | undefined;
      const pktType  = (e.details.pkt_type as string) ?? 'DATA';
      return {
        id: packetCounterRef.current,
        from: fromNode ?? '',
        to: toAddr ?? '',
        pkt_type: pktType,
        createdAt: now,
        durationMs: 500,
      };
    }).filter(a => a.from && a.to);

    if (newAnims.length > 0) {
      setRecentPackets(prev => [...prev, ...newAnims].slice(-80));
    }
  }, [events]);

  // Animation frame ticker
  const [tick, setTick] = useState(0);
  useEffect(() => {
    let animId: number;
    function frame() {
      setTick(t => t + 1);
      const now = Date.now();
      setRecentPackets(prev => {
        const alive = prev.filter(p => now - p.createdAt < p.durationMs);
        return alive.length === prev.length ? prev : alive;
      });
      animId = requestAnimationFrame(frame);
    }
    animId = requestAnimationFrame(frame);
    return () => cancelAnimationFrame(animId);
  }, []);

  void tick;

  // Links between nodes
  const links = useMemo(() => {
    const activeNodes = nodeList.filter(n => n.active);
    const result: Array<{ from: SimNode; to: SimNode; inRange: boolean }> = [];
    for (let i = 0; i < activeNodes.length; i++) {
      for (let j = i + 1; j < activeNodes.length; j++) {
        const a = activeNodes[i];
        const b = activeNodes[j];
        const dx = a.x - b.x;
        const dy = a.y - b.y;
        const dist = Math.sqrt(dx * dx + dy * dy);
        result.push({ from: a, to: b, inRange: dist <= radioRange });
      }
    }
    return result;
  }, [nodeList, radioRange]);

  const W = 800;
  const H = 500;

  const transform = useMemo(() => computeViewBox(nodeList, W, H), [nodeList]);

  const clientToSim = useCallback((clientX: number, clientY: number) => {
    const svg = svgRef.current;
    if (!svg) return { x: 0, y: 0 };
    const rect = svg.getBoundingClientRect();
    const svgX = (clientX - rect.left) / rect.width * W;
    const svgY = (clientY - rect.top) / rect.height * H;
    const scale = 'scale' in transform && transform.scale !== undefined ? transform.scale : 1;
    const simX = (svgX - transform.offsetX) / scale;
    const simY = (svgY - transform.offsetY) / scale;
    return { x: simX, y: simY };
  }, [transform]);

  const hitTestNode = useCallback((clientX: number, clientY: number): string | null => {
    const svg = svgRef.current;
    if (!svg) return null;
    const rect = svg.getBoundingClientRect();
    const svgX = (clientX - rect.left) / rect.width * W;
    const svgY = (clientY - rect.top) / rect.height * H;
    for (const node of nodeList) {
      const { sx, sy } = toScreen(node.x, node.y, transform);
      const dx = svgX - sx;
      const dy = svgY - sy;
      if (dx * dx + dy * dy <= (NODE_RADIUS + 8) * (NODE_RADIUS + 8)) {
        return node.id;
      }
    }
    return null;
  }, [nodeList, transform]);

  const TRASH_X = W - 50;
  const TRASH_Y = H - 50;
  const TRASH_R = 30;

  const isOverTrashZone = useCallback((clientX: number, clientY: number) => {
    const svg = svgRef.current;
    if (!svg) return false;
    const rect = svg.getBoundingClientRect();
    const svgX = (clientX - rect.left) / rect.width * W;
    const svgY = (clientY - rect.top) / rect.height * H;
    const dx = svgX - TRASH_X;
    const dy = svgY - TRASH_Y;
    return dx * dx + dy * dy <= TRASH_R * TRASH_R;
  }, []);

  const removeNode = useCallback((nodeId: string) => {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ type: 'remove_node', node_id: nodeId }));
    }
  }, [ws]);

  const handleMouseDown = useCallback((e: React.MouseEvent) => {
    const nodeId = hitTestNode(e.clientX, e.clientY);
    if (!nodeId) return;
    e.preventDefault();
    const node = nodes.get(nodeId);
    if (!node) return;
    dragRef.current = { nodeId, startX: e.clientX, startY: e.clientY, origX: node.x, origY: node.y };
    dragStartTime.current = Date.now();
    setDragPos({ nodeId, x: node.x, y: node.y });
  }, [hitTestNode, nodes]);

  const handleMouseMove = useCallback((e: React.MouseEvent) => {
    if (!dragRef.current) return;
    e.preventDefault();
    const sim = clientToSim(e.clientX, e.clientY);
    const startSim = clientToSim(dragRef.current.startX, dragRef.current.startY);
    const newX = dragRef.current.origX + (sim.x - startSim.x);
    const newY = dragRef.current.origY + (sim.y - startSim.y);
    setDragPos({ nodeId: dragRef.current.nodeId, x: newX, y: newY });
    setOverTrash(isOverTrashZone(e.clientX, e.clientY));
  }, [clientToSim, isOverTrashZone]);

  const handleMouseUp = useCallback((e?: React.MouseEvent) => {
    if (!dragRef.current || !dragPos) return;
    const elapsed = Date.now() - dragStartTime.current;
    const dx = e ? e.clientX - dragRef.current.startX : 0;
    const dy = e ? e.clientY - dragRef.current.startY : 0;
    const dist = Math.sqrt(dx * dx + dy * dy);

    // If short click with minimal movement, treat as click (select node)
    if (elapsed < 200 && dist < 5) {
      onNodeClick?.(dragRef.current.nodeId);
      dragRef.current = null;
      setDragPos(null);
      setOverTrash(false);
      return;
    }

    const trash = e ? isOverTrashZone(e.clientX, e.clientY) : overTrash;
    if (trash) {
      removeNode(dragRef.current.nodeId);
    } else if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({
        type: 'move_node',
        node_id: dragRef.current.nodeId,
        x: Math.round(dragPos.x * 10) / 10,
        y: Math.round(dragPos.y * 10) / 10,
      }));
    }
    dragRef.current = null;
    setDragPos(null);
    setOverTrash(false);
  }, [dragPos, ws, overTrash, isOverTrashZone, removeNode, onNodeClick]);

  // Touch handlers
  const handleTouchStart = useCallback((e: React.TouchEvent) => {
    if (e.touches.length !== 1) return;
    const touch = e.touches[0];
    const nodeId = hitTestNode(touch.clientX, touch.clientY);
    if (!nodeId) return;
    e.preventDefault();
    const node = nodes.get(nodeId);
    if (!node) return;
    dragRef.current = { nodeId, startX: touch.clientX, startY: touch.clientY, origX: node.x, origY: node.y };
    dragStartTime.current = Date.now();
    setDragPos({ nodeId, x: node.x, y: node.y });
  }, [hitTestNode, nodes]);

  const handleTouchMove = useCallback((e: React.TouchEvent) => {
    if (!dragRef.current || e.touches.length !== 1) return;
    e.preventDefault();
    const touch = e.touches[0];
    const sim = clientToSim(touch.clientX, touch.clientY);
    const startSim = clientToSim(dragRef.current.startX, dragRef.current.startY);
    const newX = dragRef.current.origX + (sim.x - startSim.x);
    const newY = dragRef.current.origY + (sim.y - startSim.y);
    setDragPos({ nodeId: dragRef.current.nodeId, x: newX, y: newY });
    setOverTrash(isOverTrashZone(touch.clientX, touch.clientY));
  }, [clientToSim, isOverTrashZone]);

  const handleTouchEnd = useCallback((e: React.TouchEvent) => {
    if (!dragRef.current || !dragPos) return;
    let trash = overTrash;
    if (e.changedTouches.length > 0) {
      const touch = e.changedTouches[0];
      trash = isOverTrashZone(touch.clientX, touch.clientY);
    }
    if (trash) {
      removeNode(dragRef.current.nodeId);
    } else if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({
        type: 'move_node',
        node_id: dragRef.current.nodeId,
        x: Math.round(dragPos.x * 10) / 10,
        y: Math.round(dragPos.y * 10) / 10,
      }));
    }
    dragRef.current = null;
    setDragPos(null);
    setOverTrash(false);
  }, [dragPos, ws, overTrash, isOverTrashZone, removeNode]);

  // Grid lines
  const gridLines: React.ReactNode[] = [];
  if (nodeList.length > 0) {
    const xs = nodeList.map(n => n.x);
    const ys = nodeList.map(n => n.y);
    const dataMinX = Math.floor(Math.min(...xs) / GRID_SIZE) * GRID_SIZE - GRID_SIZE;
    const dataMaxX = Math.ceil(Math.max(...xs) / GRID_SIZE) * GRID_SIZE + GRID_SIZE;
    const dataMinY = Math.floor(Math.min(...ys) / GRID_SIZE) * GRID_SIZE - GRID_SIZE;
    const dataMaxY = Math.ceil(Math.max(...ys) / GRID_SIZE) * GRID_SIZE + GRID_SIZE;

    for (let gx = dataMinX; gx <= dataMaxX; gx += GRID_SIZE) {
      const { sx: x1 } = toScreen(gx, dataMinY, transform);
      const { sy: y1 } = toScreen(gx, dataMinY, transform);
      const { sy: y2 } = toScreen(gx, dataMaxY, transform);
      gridLines.push(
        <line key={`gx${gx}`} x1={x1} y1={y1} x2={x1} y2={y2} stroke="#1c2128" strokeWidth="1" />
      );
    }
    for (let gy = dataMinY; gy <= dataMaxY; gy += GRID_SIZE) {
      const { sx: x1, sy: y1 } = toScreen(dataMinX, gy, transform);
      const { sx: x2 } = toScreen(dataMaxX, gy, transform);
      gridLines.push(
        <line key={`gy${gy}`} x1={x1} y1={y1} x2={x2} y2={y1} stroke="#1c2128" strokeWidth="1" />
      );
    }
  }

  // Render packet animation dots
  const now = Date.now();
  const packetDots = recentPackets.flatMap(anim => {
    const elapsed = now - anim.createdAt;
    const t = Math.min(elapsed / anim.durationMs, 1);

    const srcNode = nodes.get(anim.from);
    if (!srcNode) return [];

    let dstNode = nodes.get(anim.to) ?? resolveAddrToNode(anim.to, nodes);
    if (!dstNode && anim.to.startsWith('0x')) return [];
    if (!dstNode) return [];
    if (!dstNode.active) return [];

    const { sx: x1, sy: y1 } = toScreen(srcNode.x, srcNode.y, transform);
    const { sx: x2, sy: y2 } = toScreen(dstNode.x, dstNode.y, transform);

    const cx = x1 + (x2 - x1) * t;
    const cy = y1 + (y2 - y1) * t;
    const opacity = 1 - t * 0.3;

    const { fill, glow } = pktColor(anim.pkt_type);

    return [
      <g key={anim.id} opacity={opacity}>
        <circle cx={cx} cy={cy} r={8} fill={glow} opacity={0.35} />
        <circle cx={cx} cy={cy} r={4.5} fill={fill} />
        <text x={cx} y={cy - 9} textAnchor="middle" fontSize="7" fontFamily="monospace" fill={fill} opacity={0.85}>
          {anim.pkt_type}
        </text>
      </g>
    ];
  });

  // Render delivery path animations (green traces)
  const deliveryPathElements = deliveryPaths.flatMap(dp => {
    const elapsed = now - dp.createdAt;
    const totalDuration = dp.durationMs;
    if (elapsed >= totalDuration) return [];

    const hops = dp.path.length - 1;
    if (hops < 1) return [];

    const elements: React.ReactNode[] = [];
    const traceProgress = Math.min(elapsed / (totalDuration * 0.4), 1); // First 40% = trace animation
    const fadeProgress = elapsed > totalDuration * 0.7 ? (elapsed - totalDuration * 0.7) / (totalDuration * 0.3) : 0;
    const opacity = 1 - fadeProgress;

    // Draw path segments
    const activeHops = Math.floor(traceProgress * hops) + 1;
    for (let i = 0; i < Math.min(activeHops, hops); i++) {
      const fromId = dp.path[i];
      const toId = dp.path[i + 1];
      const fromNode = nodes.get(fromId) ?? resolveAddrToNode(fromId, nodes);
      const toNode = nodes.get(toId) ?? resolveAddrToNode(toId, nodes);
      if (!fromNode || !toNode) continue;

      const { sx: x1, sy: y1 } = toScreen(fromNode.x, fromNode.y, transform);
      const { sx: x2, sy: y2 } = toScreen(toNode.x, toNode.y, transform);

      // Glow line
      elements.push(
        <line key={`dp-glow-${dp.id}-${i}`}
          x1={x1} y1={y1} x2={x2} y2={y2}
          stroke="#3fb950" strokeWidth={5} opacity={opacity * 0.3}
          filter="url(#glow)"
        />
      );
      // Main line
      elements.push(
        <line key={`dp-line-${dp.id}-${i}`}
          x1={x1} y1={y1} x2={x2} y2={y2}
          stroke="#3fb950" strokeWidth={2.5} opacity={opacity * 0.8}
        />
      );
    }

    // Green particle at the leading edge
    if (traceProgress < 1) {
      const currentHop = Math.min(Math.floor(traceProgress * hops), hops - 1);
      const hopProgress = (traceProgress * hops) - currentHop;
      const fromId = dp.path[currentHop];
      const toId = dp.path[currentHop + 1];
      const fromNode = nodes.get(fromId) ?? resolveAddrToNode(fromId, nodes);
      const toNode = nodes.get(toId) ?? resolveAddrToNode(toId, nodes);
      if (fromNode && toNode) {
        const { sx: x1, sy: y1 } = toScreen(fromNode.x, fromNode.y, transform);
        const { sx: x2, sy: y2 } = toScreen(toNode.x, toNode.y, transform);
        const px = x1 + (x2 - x1) * hopProgress;
        const py = y1 + (y2 - y1) * hopProgress;
        elements.push(
          <g key={`dp-particle-${dp.id}`} opacity={opacity}>
            <circle cx={px} cy={py} r={10} fill="#238636" opacity={0.4} />
            <circle cx={px} cy={py} r={5} fill="#3fb950" />
          </g>
        );
      }
    }

    // Checkmark at destination when trace completes
    if (traceProgress >= 1) {
      const destId = dp.path[dp.path.length - 1];
      const destNode = nodes.get(destId) ?? resolveAddrToNode(destId, nodes);
      if (destNode) {
        const { sx, sy } = toScreen(destNode.x, destNode.y, transform);
        elements.push(
          <text key={`dp-check-${dp.id}`}
            x={sx + NODE_RADIUS + 4} y={sy - NODE_RADIUS - 2}
            fontSize="14" fill="#3fb950" opacity={opacity}
            fontWeight="bold"
          >
            ✓
          </text>
        );
      }
    }

    return elements;
  });

  // Get link quality info, RSSI-aware
  const getLinkStyle = useCallback((fromId: string, toId: string, inRange: boolean) => {
    const key = makeLinkKey(fromId, toId);

    // Check if broken
    const broken = brokenLinks?.get(key);
    if (broken) {
      const elapsed = now - broken.brokenAt;
      const fadeOpacity = Math.max(0, 1 - elapsed / BROKEN_LINK_FADE_MS);
      return {
        stroke: '#f85149',
        strokeWidth: 2,
        strokeDasharray: '6 4',
        opacity: fadeOpacity * 0.8,
        packetCount: 0,
        rssi: null as number | null,
        snr: null as number | null,
      };
    }

    const activity = linkActivity?.get(key);
    const lq = linkQuality?.get(key);
    const hasRecentTraffic = activity && (now - activity.lastActiveAt < RECENT_TRAFFIC_MS);

    // If we have RSSI data, use it to color and size the link
    if (lq && hasRecentTraffic) {
      return {
        stroke: rssiToColor(lq.rssi),
        strokeWidth: rssiToWidth(lq.rssi),
        strokeDasharray: undefined as string | undefined,
        opacity: 0.9,
        packetCount: activity.packetCount,
        rssi: lq.rssi,
        snr: lq.snr,
      };
    }

    // Active traffic but no RSSI yet
    if (hasRecentTraffic) {
      return {
        stroke: '#3fb950',
        strokeWidth: 2.5,
        strokeDasharray: undefined as string | undefined,
        opacity: 0.9,
        packetCount: activity!.packetCount,
        rssi: null as number | null,
        snr: null as number | null,
      };
    }

    // In range but no recent traffic: show faint RSSI-colored line if we have data
    if (inRange) {
      if (lq) {
        return {
          stroke: rssiToColor(lq.rssi),
          strokeWidth: Math.max(1.0, rssiToWidth(lq.rssi) * 0.5),
          strokeDasharray: undefined as string | undefined,
          opacity: 0.25,
          packetCount: activity?.packetCount ?? 0,
          rssi: lq.rssi,
          snr: lq.snr,
        };
      }
      return {
        stroke: '#238636',
        strokeWidth: 1.5,
        strokeDasharray: undefined as string | undefined,
        opacity: 0.4,
        packetCount: activity?.packetCount ?? 0,
        rssi: null as number | null,
        snr: null as number | null,
      };
    }

    // Out of range
    return {
      stroke: '#21262d',
      strokeWidth: 1,
      strokeDasharray: '4 3',
      opacity: 0.3,
      packetCount: 0,
      rssi: null as number | null,
      snr: null as number | null,
    };
  }, [linkActivity, brokenLinks, linkQuality, now]);

  return (
    <div data-testid="mesh-canvas" style={{
      width: '100%',
      height: '100%',
      background: '#0d1117',
      position: 'relative',
      overflow: 'hidden',
      borderRadius: '4px',
    }}>
      <svg
        ref={svgRef}
        width="100%"
        height="100%"
        viewBox={`0 0 ${W} ${H}`}
        preserveAspectRatio="xMidYMid meet"
        style={{ display: 'block', cursor: dragRef.current ? 'grabbing' : 'default', touchAction: 'none' }}
        onMouseDown={handleMouseDown}
        onMouseMove={handleMouseMove}
        onMouseUp={handleMouseUp}
        onMouseLeave={handleMouseUp}
        onTouchStart={handleTouchStart}
        onTouchMove={handleTouchMove}
        onTouchEnd={handleTouchEnd}
        onTouchCancel={() => { dragRef.current = null; setDragPos(null); setOverTrash(false); }}
        onContextMenu={(e) => {
          e.preventDefault();
          const nodeId = hitTestNode(e.clientX, e.clientY);
          if (nodeId) {
            const svg = svgRef.current;
            if (!svg) return;
            const rect = svg.getBoundingClientRect();
            setContextMenu({ nodeId, x: e.clientX - rect.left, y: e.clientY - rect.top });
          } else {
            setContextMenu(null);
          }
        }}
        onClick={(e) => {
          setContextMenu(null);
          // Click on empty space deselects
          if (!hitTestNode(e.clientX, e.clientY) && !dragRef.current) {
            onNodeClick?.('' /* empty = deselect, handled in parent */);
          }
        }}
      >
        <defs>
          <filter id="glow" x="-50%" y="-50%" width="200%" height="200%">
            <feGaussianBlur stdDeviation="2.5" result="blur" />
            <feMerge>
              <feMergeNode in="blur" />
              <feMergeNode in="SourceGraphic" />
            </feMerge>
          </filter>
          <filter id="greenGlow" x="-50%" y="-50%" width="200%" height="200%">
            <feGaussianBlur stdDeviation="3" result="blur" />
            <feMerge>
              <feMergeNode in="blur" />
              <feMergeNode in="SourceGraphic" />
            </feMerge>
          </filter>
        </defs>

        <rect width={W} height={H} fill="#0d1117" />

        {gridLines}

        {/* Link lines with RSSI-quality indicators */}
        {links.map(({ from, to, inRange }, i) => {
          const { sx: x1, sy: y1 } = toScreen(from.x, from.y, transform);
          const { sx: x2, sy: y2 } = toScreen(to.x, to.y, transform);
          const style = getLinkStyle(from.id, to.id, inRange);
          const mx = (x1 + x2) / 2;
          const my = (y1 + y2) / 2;
          const isRecentlyActive = inRange && (now - (linkActivity?.get(makeLinkKey(from.id, to.id))?.lastActiveAt ?? 0) < RECENT_TRAFFIC_MS);

          return (
            <g key={i}>
              <line
                x1={x1} y1={y1} x2={x2} y2={y2}
                stroke={style.stroke}
                strokeWidth={style.strokeWidth}
                strokeDasharray={style.strokeDasharray}
                opacity={style.opacity}
              />
              {/* RSSI label at link midpoint (toggle-able) */}
              {showRssiLabels && style.rssi !== null && inRange && (
                <text
                  x={mx} y={my - 5}
                  textAnchor="middle"
                  fontSize="8"
                  fontFamily="monospace"
                  fill={style.stroke}
                  opacity={0.85}
                >
                  {Math.round(style.rssi)}
                </text>
              )}
              {/* Packet count label on active links (when RSSI labels are off) */}
              {!showRssiLabels && style.packetCount > 0 && isRecentlyActive && (
                <text
                  x={mx} y={my - 4}
                  textAnchor="middle"
                  fontSize="8"
                  fontFamily="monospace"
                  fill="#3fb950"
                  opacity={0.7}
                >
                  {style.packetCount}
                </text>
              )}
            </g>
          );
        })}

        {/* Delivery path traces */}
        {deliveryPathElements}

        {/* Packet animation dots */}
        {packetDots}

        {/* Nodes */}
        {nodeList.map(node => {
          const isDragging = dragPos && dragPos.nodeId === node.id;
          const isSelected = selectedNodeId === node.id;
          const nx = isDragging ? dragPos.x : node.x;
          const ny = isDragging ? dragPos.y : node.y;
          const { sx, sy } = toScreen(nx, ny, transform);
          const active = node.active;
          const fill = isDragging ? '#1f6feb' : isSelected ? '#1f6feb' : (active ? '#238636' : '#21262d');
          const stroke = isDragging ? '#58a6ff' : isSelected ? '#58a6ff' : (active ? '#3fb950' : '#30363d');
          const textColor = active ? '#e6edf3' : '#6e7681';

          return (
            <g key={node.id}>
              {active && radioRange > 0 && (() => {
                const scale = 'scale' in transform && transform.scale !== undefined ? transform.scale : 1;
                return (
                  <circle
                    cx={sx} cy={sy}
                    r={radioRange * scale}
                    fill="none"
                    stroke="#238636"
                    strokeWidth="1"
                    strokeDasharray="3 4"
                    opacity="0.15"
                  />
                );
              })()}

              <circle
                cx={sx} cy={sy}
                r={NODE_RADIUS}
                fill={fill}
                stroke={stroke}
                strokeWidth={isDragging || isSelected ? 3 : 2}
                style={{ cursor: 'grab' }}
              />

              {active && (
                <circle
                  cx={sx} cy={sy}
                  r={NODE_RADIUS + 4}
                  fill="none"
                  stroke={isSelected ? '#58a6ff' : '#3fb950'}
                  strokeWidth={isSelected ? 2 : 1}
                  opacity={isSelected ? 0.6 : 0.3}
                />
              )}

              <text
                x={sx} y={sy}
                textAnchor="middle"
                dominantBaseline="central"
                fontSize="11"
                fontWeight="600"
                fontFamily="monospace"
                fill={textColor}
              >
                {node.id}
              </text>

              <text
                x={sx} y={sy + NODE_RADIUS + 12}
                textAnchor="middle"
                fontSize="9"
                fontFamily="monospace"
                fill="#6e7681"
              >
                ({Math.round(node.x)},{Math.round(node.y)})
              </text>
            </g>
          );
        })}

        {/* Trash zone */}
        {dragPos && (
          <g>
            <circle
              cx={TRASH_X} cy={TRASH_Y} r={TRASH_R}
              fill={overTrash ? 'rgba(248,81,73,0.3)' : 'rgba(110,118,129,0.15)'}
              stroke={overTrash ? '#f85149' : '#6e7681'}
              strokeWidth={overTrash ? 2 : 1}
              strokeDasharray={overTrash ? undefined : '4 3'}
            />
            <text
              x={TRASH_X} y={TRASH_Y - 2}
              textAnchor="middle" dominantBaseline="central"
              fontSize={overTrash ? '18' : '16'} fill={overTrash ? '#f85149' : '#6e7681'}
            >
              🗑
            </text>
            {overTrash && (
              <text
                x={TRASH_X} y={TRASH_Y + 18}
                textAnchor="middle" fontSize="8" fill="#f85149" fontFamily="monospace"
              >
                Drop to delete
              </text>
            )}
          </g>
        )}

        {nodeList.length === 0 && (
          <text
            x={W / 2} y={H / 2}
            textAnchor="middle"
            dominantBaseline="central"
            fontSize="14"
            fill="#6e7681"
            fontFamily="monospace"
          >
            Waiting for nodes...
          </text>
        )}
      </svg>

      {/* RSSI toggle button */}
      <button
        onClick={() => setShowRssiLabels(v => !v)}
        title="Toggle RSSI labels on links"
        style={{
          position: 'absolute',
          top: 8,
          right: 8,
          background: showRssiLabels ? '#1f6feb' : '#21262d',
          border: `1px solid ${showRssiLabels ? '#58a6ff' : '#30363d'}`,
          borderRadius: '4px',
          color: showRssiLabels ? '#e6edf3' : '#8b949e',
          fontSize: '10px',
          fontFamily: 'monospace',
          padding: '3px 7px',
          cursor: 'pointer',
        }}
      >
        dBm {showRssiLabels ? '●' : '○'}
      </button>

      {/* RSSI legend */}
      <div style={{
        position: 'absolute',
        bottom: 8,
        left: 8,
        background: 'rgba(13,17,23,0.85)',
        border: '1px solid #21262d',
        borderRadius: '5px',
        padding: '5px 8px',
        fontFamily: 'monospace',
        fontSize: '9px',
        color: '#8b949e',
        pointerEvents: 'none',
      }}>
        <div style={{ marginBottom: '3px', color: '#6e7681', fontWeight: 600, letterSpacing: '0.05em' }}>
          RSSI
        </div>
        {[
          { label: '&gt; −70', color: '#00ff88', desc: 'Strong' },
          { label: '−85', color: '#c8e838', desc: 'Good' },
          { label: '−100', color: '#f0883e', desc: 'Fair' },
          { label: '&lt; −100', color: '#f85149', desc: 'Weak' },
        ].map(({ label, color, desc }) => (
          <div key={desc} style={{ display: 'flex', alignItems: 'center', gap: '5px', marginTop: '2px' }}>
            <div style={{ width: 22, height: 3, background: color, borderRadius: 2, flexShrink: 0 }} />
            <span style={{ color: '#8b949e' }}>{desc}</span>
            <span style={{ color: '#484f58', marginLeft: 'auto' }} dangerouslySetInnerHTML={{ __html: label }} />
          </div>
        ))}
      </div>

      {/* Right-click context menu */}
      {contextMenu && (
        <div
          style={{
            position: 'absolute',
            left: contextMenu.x,
            top: contextMenu.y,
            background: '#161b22',
            border: '1px solid #30363d',
            borderRadius: '6px',
            padding: '4px 0',
            zIndex: 100,
            boxShadow: '0 8px 24px rgba(0,0,0,0.4)',
            minWidth: '120px',
          }}
        >
          <div
            style={{
              padding: '6px 12px',
              fontSize: '12px',
              color: '#8b949e',
              fontFamily: 'monospace',
              borderBottom: '1px solid #21262d',
            }}
          >
            {contextMenu.nodeId}
          </div>
          <button
            onClick={() => { onNodeClick?.(contextMenu.nodeId); setContextMenu(null); }}
            style={{
              display: 'block',
              width: '100%',
              padding: '6px 12px',
              background: 'none',
              border: 'none',
              color: '#58a6ff',
              fontSize: '12px',
              fontFamily: 'monospace',
              cursor: 'pointer',
              textAlign: 'left',
            }}
            onMouseEnter={(e) => (e.currentTarget.style.background = '#21262d')}
            onMouseLeave={(e) => (e.currentTarget.style.background = 'none')}
          >
            📋 Inspect Node
          </button>
          <button
            onClick={() => { removeNode(contextMenu.nodeId); setContextMenu(null); }}
            style={{
              display: 'block',
              width: '100%',
              padding: '6px 12px',
              background: 'none',
              border: 'none',
              color: '#f85149',
              fontSize: '12px',
              fontFamily: 'monospace',
              cursor: 'pointer',
              textAlign: 'left',
            }}
            onMouseEnter={(e) => (e.currentTarget.style.background = '#21262d')}
            onMouseLeave={(e) => (e.currentTarget.style.background = 'none')}
          >
            🗑 Delete Node
          </button>
        </div>
      )}
    </div>
  );
}
