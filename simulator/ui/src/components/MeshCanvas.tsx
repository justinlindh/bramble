import React, { useMemo } from 'react';
import type { SimNode } from '../types';

interface MeshCanvasProps {
  nodes: Map<string, SimNode>;
  radioRange?: number;
}

const PADDING = 60;
const NODE_RADIUS = 18;
const GRID_SIZE = 50;

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

export function MeshCanvas({ nodes, radioRange = 150 }: MeshCanvasProps) {
  const nodeList = useMemo(() => Array.from(nodes.values()), [nodes]);

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

  // Use a ResizeObserver-friendly approach: use a fixed internal coordinate system
  const W = 800;
  const H = 500;

  const transform = useMemo(() => computeViewBox(nodeList, W, H), [nodeList]);

  // Grid lines in data space — we draw in screen space
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

  return (
    <div style={{
      flex: 1,
      background: '#0d1117',
      position: 'relative',
      overflow: 'hidden',
      borderRadius: '4px',
    }}>
      <svg
        width="100%"
        height="100%"
        viewBox={`0 0 ${W} ${H}`}
        preserveAspectRatio="xMidYMid meet"
        style={{ display: 'block' }}
      >
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

        {/* Nodes */}
        {nodeList.map(node => {
          const { sx, sy } = toScreen(node.x, node.y, transform);
          const active = node.active;
          const fill = active ? '#238636' : '#21262d';
          const stroke = active ? '#3fb950' : '#30363d';
          const textColor = active ? '#e6edf3' : '#6e7681';

          return (
            <g key={node.id}>
              {/* Radio range circle (subtle) */}
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
                strokeWidth="2"
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
