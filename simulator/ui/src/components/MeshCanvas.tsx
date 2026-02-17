import React, { useMemo, useEffect, useRef, useState, useCallback } from 'react';
import type { SimNode, SimEvent, PacketAnimation } from '../types';

interface MeshCanvasProps {
  nodes: Map<string, SimNode>;
  radioRange?: number;
  events?: SimEvent[];
  ws?: WebSocket | null;
}

const PADDING = 60;
const NODE_RADIUS = 18;
const GRID_SIZE = 50;

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

export function MeshCanvas({ nodes, radioRange = 150, events = [], ws }: MeshCanvasProps) {
  const nodeList = useMemo(() => Array.from(nodes.values()), [nodes]);
  const svgRef = useRef<SVGSVGElement>(null);
  const dragRef = useRef<{ nodeId: string; startX: number; startY: number; origX: number; origY: number } | null>(null);
  const [dragPos, setDragPos] = useState<{ nodeId: string; x: number; y: number } | null>(null);

  // Build recent packet animations from events
  const [recentPackets, setRecentPackets] = useState<PacketAnimation[]>([]);
  const packetCounterRef = useRef(0);

  // Expire stale animations when events list changes
  useEffect(() => {
    const now = Date.now();
    setRecentPackets(prev => {
      const alive = prev.filter(p => now - p.createdAt < p.durationMs);
      return alive;
    });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [events]);

  // Listen for new packet_sent events
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

  // Animation frame ticker to keep dots moving
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

  void tick; // used to trigger re-render

  // Determine if nodes are in radio range of each other
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

  // Convert a client (mouse/touch) position to sim coordinates
  const clientToSim = useCallback((clientX: number, clientY: number) => {
    const svg = svgRef.current;
    if (!svg) return { x: 0, y: 0 };
    const rect = svg.getBoundingClientRect();
    // Map client coords to SVG viewBox coords
    const svgX = (clientX - rect.left) / rect.width * W;
    const svgY = (clientY - rect.top) / rect.height * H;
    // Invert the transform: screenPos = simPos * scale + offset
    const scale = 'scale' in transform && transform.scale !== undefined ? transform.scale : 1;
    const simX = (svgX - transform.offsetX) / scale;
    const simY = (svgY - transform.offsetY) / scale;
    return { x: simX, y: simY };
  }, [transform]);

  // Find which node is at a screen position (within NODE_RADIUS)
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

  // Drag handlers (mouse)
  const handleMouseDown = useCallback((e: React.MouseEvent) => {
    const nodeId = hitTestNode(e.clientX, e.clientY);
    if (!nodeId) return;
    e.preventDefault();
    const node = nodes.get(nodeId);
    if (!node) return;
    dragRef.current = { nodeId, startX: e.clientX, startY: e.clientY, origX: node.x, origY: node.y };
    setDragPos({ nodeId, x: node.x, y: node.y });
  }, [hitTestNode, clientToSim, nodes]);

  const handleMouseMove = useCallback((e: React.MouseEvent) => {
    if (!dragRef.current) return;
    e.preventDefault();
    const sim = clientToSim(e.clientX, e.clientY);
    const startSim = clientToSim(dragRef.current.startX, dragRef.current.startY);
    const newX = dragRef.current.origX + (sim.x - startSim.x);
    const newY = dragRef.current.origY + (sim.y - startSim.y);
    setDragPos({ nodeId: dragRef.current.nodeId, x: newX, y: newY });
  }, [clientToSim]);

  const handleMouseUp = useCallback(() => {
    if (!dragRef.current || !dragPos) return;
    // Send move_node command
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({
        type: 'move_node',
        node_id: dragRef.current.nodeId,
        x: Math.round(dragPos.x * 10) / 10,
        y: Math.round(dragPos.y * 10) / 10,
      }));
    }
    dragRef.current = null;
    setDragPos(null);
  }, [dragPos, ws]);

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
  }, [clientToSim]);

  const handleTouchEnd = useCallback(() => {
    if (!dragRef.current || !dragPos) return;
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({
        type: 'move_node',
        node_id: dragRef.current.nodeId,
        x: Math.round(dragPos.x * 10) / 10,
        y: Math.round(dragPos.y * 10) / 10,
      }));
    }
    dragRef.current = null;
    setDragPos(null);
  }, [dragPos, ws]);

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

    // Find source node
    const srcNode = nodes.get(anim.from);
    if (!srcNode) return [];

    // Find dest node by id or resolve address
    let dstNode = nodes.get(anim.to) ?? resolveAddrToNode(anim.to, nodes);

    // If dest not found by node id, try to find by matching node addresses
    // The dest might be a hex address string like "0x02000003"
    if (!dstNode && anim.to.startsWith('0x')) {
      // Can't resolve without addr in node state; skip
      return [];
    }

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
        {/* Glow */}
        <circle cx={cx} cy={cy} r={8} fill={glow} opacity={0.35} />
        {/* Dot */}
        <circle cx={cx} cy={cy} r={4.5} fill={fill} />
        {/* Label */}
        <text
          x={cx}
          y={cy - 9}
          textAnchor="middle"
          fontSize="7"
          fontFamily="monospace"
          fill={fill}
          opacity={0.85}
        >
          {anim.pkt_type}
        </text>
      </g>
    ];
  });

  return (
    <div style={{
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
        onTouchCancel={handleTouchEnd}
      >
        {/* SVG filter for glow */}
        <defs>
          <filter id="glow" x="-50%" y="-50%" width="200%" height="200%">
            <feGaussianBlur stdDeviation="2.5" result="blur" />
            <feMerge>
              <feMergeNode in="blur" />
              <feMergeNode in="SourceGraphic" />
            </feMerge>
          </filter>
        </defs>

        {/* Background */}
        <rect width={W} height={H} fill="#0d1117" />

        {/* Grid */}
        {gridLines}

        {/* Link lines */}
        {links.map(({ from, to, inRange }, i) => {
          const { sx: x1, sy: y1 } = toScreen(from.x, from.y, transform);
          const { sx: x2, sy: y2 } = toScreen(to.x, to.y, transform);
          return (
            <line
              key={i}
              x1={x1} y1={y1} x2={x2} y2={y2}
              stroke={inRange ? '#238636' : '#21262d'}
              strokeWidth={inRange ? 1.5 : 1}
              strokeDasharray={inRange ? undefined : '4 3'}
              opacity={inRange ? 0.7 : 0.3}
            />
          );
        })}

        {/* Packet animation dots (below nodes so nodes render on top) */}
        {packetDots}

        {/* Nodes */}
        {nodeList.map(node => {
          const isDragging = dragPos && dragPos.nodeId === node.id;
          const nx = isDragging ? dragPos.x : node.x;
          const ny = isDragging ? dragPos.y : node.y;
          const { sx, sy } = toScreen(nx, ny, transform);
          const active = node.active;
          const fill = isDragging ? '#1f6feb' : (active ? '#238636' : '#21262d');
          const stroke = isDragging ? '#58a6ff' : (active ? '#3fb950' : '#30363d');
          const textColor = active ? '#e6edf3' : '#6e7681';

          return (
            <g key={node.id}>
              {/* Radio range circle */}
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

              {/* Node circle */}
              <circle
                cx={sx} cy={sy}
                r={NODE_RADIUS}
                fill={fill}
                stroke={stroke}
                strokeWidth={isDragging ? 3 : 2}
                style={{ cursor: 'grab' }}
              />

              {/* Pulse ring for active nodes */}
              {active && (
                <circle
                  cx={sx} cy={sy}
                  r={NODE_RADIUS + 4}
                  fill="none"
                  stroke="#3fb950"
                  strokeWidth="1"
                  opacity="0.3"
                />
              )}

              {/* ID label */}
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

              {/* Position label below */}
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

        {/* Empty state */}
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
    </div>
  );
}
