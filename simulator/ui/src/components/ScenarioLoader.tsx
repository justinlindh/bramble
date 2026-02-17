import React, { useEffect, useState } from 'react';

interface ScenarioLoaderProps {
  onLoad?: (scenario: string) => void;
}

export function ScenarioLoader({ onLoad }: ScenarioLoaderProps) {
  const [scenarios, setScenarios] = useState<string[]>([]);
  const [selected, setSelected] = useState<string>('');
  const [loading, setLoading] = useState(false);

  useEffect(() => {
    fetch('/api/scenarios')
      .then(r => r.json())
      .then((data: string[]) => {
        setScenarios(data);
        if (data.length > 0) setSelected(data[0]);
      })
      .catch(() => {
        setScenarios(['test-2-node']);
        setSelected('test-2-node');
      });
  }, []);

  function handleLoad() {
    if (!selected) return;
    setLoading(true);
    onLoad?.(selected);
    setTimeout(() => setLoading(false), 1000);
  }

  return (
    <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
      <select
        value={selected}
        onChange={e => setSelected(e.target.value)}
        style={{
          background: '#21262d',
          border: '1px solid #30363d',
          borderRadius: '6px',
          color: '#e6edf3',
          padding: '4px 10px',
          fontSize: '12px',
          cursor: 'pointer',
          outline: 'none',
        }}
      >
        {scenarios.map(s => (
          <option key={s} value={s}>{s}</option>
        ))}
      </select>
      <button
        onClick={handleLoad}
        disabled={loading || !selected}
        style={{
          background: loading ? '#21262d' : '#238636',
          border: '1px solid ' + (loading ? '#30363d' : '#2ea043'),
          borderRadius: '6px',
          color: loading ? '#8b949e' : '#fff',
          padding: '4px 12px',
          fontSize: '12px',
          cursor: loading ? 'not-allowed' : 'pointer',
          fontWeight: 500,
          transition: 'background 0.2s',
        }}
      >
        {loading ? 'Loading...' : 'Load'}
      </button>
    </div>
  );
}
