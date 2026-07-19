import { describe, expect, it } from 'vitest';
import { cspPlugin, DEVELOPMENT_CSP, PRODUCTION_CSP } from '../../csp.config';

/**
 * Locks down the actual Content-Security-Policy content, not just the fact
 * that a policy exists. See docs issue: a directive change that quietly
 * widens script-src, or drops an allowance something in the app relies on,
 * should show up here as a red diff instead of shipping silently.
 */

/** Splits a `; `-joined CSP string into a directive-name -> full-value map. */
function parseDirectives(csp: string): Record<string, string> {
  const out: Record<string, string> = {};
  for (const directive of csp.split('; ')) {
    const [name] = directive.split(' ', 1);
    out[name] = directive;
  }
  return out;
}

describe('PRODUCTION_CSP', () => {
  const directives = parseDirectives(PRODUCTION_CSP);

  it('locks script-src to self with no inline or eval script', () => {
    // This is the property most worth defending: the easiest thing to lose
    // by accident when someone is debugging a build issue and loosens
    // script-src "just for now."
    expect(directives['script-src']).toBe("script-src 'self'");
  });

  it('does not contain unsafe-inline or unsafe-eval anywhere in the policy', () => {
    expect(PRODUCTION_CSP).not.toContain('unsafe-eval');
    // style-src legitimately carries unsafe-inline (asserted below); the
    // regression this guards against is script-src acquiring it too.
    expect(directives['script-src']).not.toContain('unsafe-inline');
  });

  it('allows data: images and the OSM tile host for the Map page and inlined Leaflet markers', () => {
    expect(directives['img-src']).toBe(
      "img-src 'self' data: https://*.tile.openstreetmap.org"
    );
  });

  it('keeps unsafe-inline in style-src for Leaflet inline style attributes', () => {
    expect(directives['style-src']).toBe("style-src 'self' 'unsafe-inline'");
  });

  it('pins the base security directives', () => {
    expect(directives['default-src']).toBe("default-src 'self'");
    expect(directives['base-uri']).toBe("base-uri 'self'");
    expect(directives['object-src']).toBe("object-src 'none'");
    expect(directives['frame-src']).toBe("frame-src 'none'");
    expect(directives['form-action']).toBe("form-action 'none'");
    expect(directives['font-src']).toBe("font-src 'self'");
  });

  it('keeps connect-src broad for the WiFi transport, hosted proxy, and OTA fetch', () => {
    expect(directives['connect-src']).toBe("connect-src 'self' ws: wss: http: https:");
  });

  it('never sets frame-ancestors, which a meta tag cannot enforce', () => {
    expect(directives['frame-ancestors']).toBeUndefined();
  });
});

describe('DEVELOPMENT_CSP', () => {
  const directives = parseDirectives(DEVELOPMENT_CSP);

  it('is the relaxed policy: script-src allows inline and eval for the Vite dev preamble', () => {
    expect(directives['script-src']).toBe("script-src 'self' 'unsafe-inline' 'unsafe-eval'");
  });

  it('shares every other directive with production (only script-src differs)', () => {
    const prodDirectives = parseDirectives(PRODUCTION_CSP);
    // Symmetric: the same directive NAMES on both sides, so development
    // gaining an extra directive production lacks also fails, not just a
    // value mismatch on a shared name.
    expect(Object.keys(directives).sort()).toEqual(Object.keys(prodDirectives).sort());
    for (const name of Object.keys(prodDirectives)) {
      if (name === 'script-src') continue;
      expect(directives[name]).toBe(prodDirectives[name]);
    }
  });
});

describe('cspPlugin()', () => {
  it('injects the strict production policy for a production build', () => {
    const plugin = cspPlugin();
    plugin.configResolved({ command: 'build' });
    const tags = plugin.transformIndexHtml();

    expect(tags).toHaveLength(1);
    expect(tags[0].tag).toBe('meta');
    expect(tags[0].attrs['http-equiv']).toBe('Content-Security-Policy');
    expect(tags[0].attrs.content).toBe(PRODUCTION_CSP);
  });

  it('injects the relaxed development policy for the dev server', () => {
    const plugin = cspPlugin();
    plugin.configResolved({ command: 'serve' });
    const tags = plugin.transformIndexHtml();

    expect(tags[0].attrs.content).toBe(DEVELOPMENT_CSP);
  });

  it('injects into head (after existing children, so charset stays first) rather than head-prepend', () => {
    const plugin = cspPlugin();
    plugin.configResolved({ command: 'build' });
    const tags = plugin.transformIndexHtml();

    expect(tags[0].injectTo).toBe('head');
  });
});
