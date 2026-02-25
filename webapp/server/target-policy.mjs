import net from 'node:net';

function ipToInt(ip) {
  const parts = ip.split('.').map((part) => Number.parseInt(part, 10));
  if (parts.length !== 4 || parts.some((part) => Number.isNaN(part) || part < 0 || part > 255)) {
    return null;
  }

  return (((parts[0] << 24) >>> 0) | (parts[1] << 16) | (parts[2] << 8) | parts[3]) >>> 0;
}

function parseCIDR(cidr) {
  const [base, prefixText] = cidr.split('/');
  if (!base || prefixText === undefined || net.isIP(base) !== 4) return null;

  const prefix = Number.parseInt(prefixText, 10);
  if (Number.isNaN(prefix) || prefix < 0 || prefix > 32) return null;

  const baseInt = ipToInt(base);
  if (baseInt === null) return null;

  const mask = prefix === 0 ? 0 : (0xffffffff << (32 - prefix)) >>> 0;
  return { base: baseInt & mask, mask };
}

function isIPv4Private(ip) {
  const value = ipToInt(ip);
  if (value === null) return false;

  const inRange = (base, maskBits) => {
    const mask = (0xffffffff << (32 - maskBits)) >>> 0;
    return ((value & mask) >>> 0) === ((base & mask) >>> 0);
  };

  return (
    inRange(ipToInt('10.0.0.0'), 8) ||
    inRange(ipToInt('172.16.0.0'), 12) ||
    inRange(ipToInt('192.168.0.0'), 16)
  );
}

export function parseAllowlist(value = process.env.ALLOWED_SUBNETS || '') {
  return value
    .split(',')
    .map((entry) => entry.trim())
    .filter(Boolean)
    .map(parseCIDR)
    .filter(Boolean);
}

export function isAllowedTarget(target, { mode = 'hosted', allowlist = [] } = {}) {
  if (mode !== 'local') return false;
  if (!target || net.isIP(target) !== 4) return false;

  if (isIPv4Private(target)) return true;

  const value = ipToInt(target);
  if (value === null) return false;

  return allowlist.some(({ base, mask }) => (value & mask) === base);
}

export function splitTarget(input) {
  const target = `${input || ''}`.trim();
  if (!target) return null;

  const match = target.match(/^(?<host>[^:]+)(?::(?<port>\d{1,5}))?$/);
  if (!match?.groups?.host) return null;

  const host = match.groups.host;
  const port = match.groups.port ? Number.parseInt(match.groups.port, 10) : null;

  if (port !== null && (Number.isNaN(port) || port < 1 || port > 65535)) return null;

  return { host, port };
}
