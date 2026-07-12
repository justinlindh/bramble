import { useEffect, useRef, useState } from 'react';

interface ScenarioLoaderProps {
  onLoad?: (scenario: string) => void;
  onScenariosRefresh?: () => void;
}

export function ScenarioLoader({ onLoad }: ScenarioLoaderProps) {
  const [scenarios, setScenarios] = useState<string[]>([]);
  const [selected, setSelected] = useState<string>('');
  const [loading, setLoading] = useState(false);
  const [uploading, setUploading] = useState(false);
  const [uploadMsg, setUploadMsg] = useState<string>('');
  const fileInputRef = useRef<HTMLInputElement>(null);

  function fetchScenarios() {
    fetch('/api/scenarios')
      .then(r => r.json())
      .then((data: string[]) => {
        setScenarios(data);
        if (data.length > 0 && !selected) setSelected(data[0]);
      })
      .catch(() => {
        setScenarios(['test-2-node']);
        setSelected('test-2-node');
      });
  }

  useEffect(() => {
    fetchScenarios();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  function handleLoad() {
    if (!selected) return;
    setLoading(true);
    onLoad?.(selected);
    setTimeout(() => setLoading(false), 1000);
  }

  function handleUploadClick() {
    fileInputRef.current?.click();
  }

  async function handleFileChange(e: React.ChangeEvent<HTMLInputElement>) {
    const file = e.target.files?.[0];
    if (!file) return;

    setUploading(true);
    setUploadMsg('');

    try {
      const text = await file.text();
      // Validate JSON
      JSON.parse(text);

      const res = await fetch('/api/scenarios/upload', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: text,
      });

      if (!res.ok) {
        const err = await res.json() as { error?: string };
        setUploadMsg(`Error: ${err.error ?? res.statusText}`);
        return;
      }

      const result = await res.json() as { name: string };
      setUploadMsg(`Uploaded: ${result.name}`);

      // Refresh the dropdown and auto-select the new scenario
      fetchScenarios();
      setTimeout(() => {
        setSelected(result.name);
        setUploadMsg('');
      }, 1500);
    } catch (err) {
      setUploadMsg(`Error: ${String(err)}`);
    } finally {
      setUploading(false);
      // Reset file input so same file can be re-uploaded
      if (fileInputRef.current) fileInputRef.current.value = '';
    }
  }

  const btnStyle: React.CSSProperties = {
    background: '#21262d',
    border: '1px solid #30363d',
    borderRadius: '6px',
    color: '#e6edf3',
    padding: '4px 10px',
    fontSize: '12px',
    cursor: 'pointer',
    outline: 'none',
  };

  return (
    <div style={{ display: 'flex', alignItems: 'center', gap: '8px', flexWrap: 'wrap' }}>
      {/* Hidden file input */}
      <input
        ref={fileInputRef}
        type="file"
        accept=".json"
        style={{ display: 'none' }}
        onChange={handleFileChange}
      />

      <select
        data-testid="scenario-select"
        value={selected}
        onChange={e => setSelected(e.target.value)}
        style={{
          ...btnStyle,
          padding: '4px 10px',
        }}
      >
        {scenarios.map(s => (
          <option key={s} value={s}>{s}</option>
        ))}
      </select>

      <button
        data-testid="scenario-load-btn"
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

      <button
        onClick={handleUploadClick}
        disabled={uploading}
        title="Upload a .json scenario file"
        style={{
          ...btnStyle,
          cursor: uploading ? 'not-allowed' : 'pointer',
          opacity: uploading ? 0.5 : 1,
          padding: '4px 10px',
          display: 'flex',
          alignItems: 'center',
          gap: '4px',
        }}
      >
        <span style={{ fontSize: '11px' }}>⬆</span>
        {uploading ? 'Uploading…' : 'Upload'}
      </button>

      {uploadMsg && (
        <span style={{
          fontSize: '11px',
          color: uploadMsg.startsWith('Error') ? '#f85149' : '#3fb950',
          fontFamily: 'monospace',
        }}>
          {uploadMsg}
        </span>
      )}
    </div>
  );
}
