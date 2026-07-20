# Bramble Color Scheme

## Overview

Bramble uses a unified dark theme across both the web app and T-Deck Plus firmware. The color scheme is inspired by GitHub's dark theme, featuring a dark blue-gray background with green accent colors.

## Color Palette

### Web App (CSS Variables)

Defined in `webapp/src/styles/global.css`:

| Variable | Hex Code | Usage |
| ---------- | ---------- | ------- |
| `--bg` | `#0d1117` | Main background |
| `--surface` | `#161b22` | Panels, cards |
| `--surface-2` | `#21262d` | Secondary surfaces, inputs |
| `--border` | `#30363d` | Borders, dividers |
| `--text` | `#e6edf3` | Primary text |
| `--text-muted` | `#8b949e` | Secondary text, labels |
| `--accent` | `#238636` | Primary accent (green) |
| `--accent-blue` | `#1f6feb` | Links, secondary accent |
| `--danger` | `#da3633` | Error states, destructive actions |
| `--warning` | `#e3b341` | Warning states |
| `--critical` | `#bc8cff` | Critical alerts |

### T-Deck Plus Firmware (LVGL)

Defined in `components/ui_graphics/theme/bramble_theme.h`:

| Constant | Hex Code | Usage |
| ---------- | ---------- | ------- |
| `BR_COLOR_BG` | `0x0D1117` | Screen background |
| `BR_COLOR_SURFACE` | `0x161B22` | Cards, panels |
| `BR_COLOR_SURFACE_2` | `0x21262D` | Secondary surfaces |
| `BR_COLOR_BORDER` | `0x30363D` | Borders, separators |
| `BR_COLOR_PRIMARY` | `0x238636` | Active tabs, buttons, primary actions |
| `BR_COLOR_ACCENT` | `0x1F6FEB` | Secondary accent, links |
| `BR_COLOR_TEXT` | `0xE6EDF3` | Primary text |
| `BR_COLOR_TEXT_SEC` | `0x8B949E` | Secondary text, timestamps |
| `BR_COLOR_SENT` | `0x1A4B91` | Sent message bubbles (blue) |
| `BR_COLOR_ON_SENT` | `0xE6EDF3` | Muted marks drawn on a sent bubble |
| `BR_COLOR_RECV` | `0x21262D` | Received message bubbles |
| `BR_COLOR_DANGER` | `0xDA3633` | Error indicators |
| `BR_COLOR_SUCCESS` | `0x238636` | Success states (matches primary) |
| `BR_COLOR_WARNING` | `0xE3B341` | Warning indicators |
| `BR_COLOR_CRITICAL` | `0xBC8CFF` | Critical alerts |

### Additional UI Colors

Some UI elements use variations of the theme colors for specific states:

| Usage | Hex Code | Description |
|-------|----------|-------------|
| Button pressed state | `0x1A6628` | Darker shade of primary green for visual feedback |

## Design Principles

1. **Dark-first**: All backgrounds are dark to reduce eye strain and conserve battery on OLED displays
2. **High contrast**: Text colors provide sufficient contrast for readability
3. **Green accent**: The primary accent color (`#238636`) is used for:
   - Active states
   - Primary actions
   - Success indicators
   (Sent message bubbles are blue, `#1A4B91`, so outgoing messages read
   distinctly from action/success accents.)
4. **Blue for links**: Secondary accent (`#1f6feb`) is reserved for hyperlinks and secondary interactive elements
5. **Semantic colors**: Red for danger/errors, yellow for warnings, purple for critical states

## Usage Examples

### Active Tab Indicator

Uses `BR_COLOR_PRIMARY` (#238636) for the active tab background tint

### Message Bubbles

- Sent messages: `BR_COLOR_SENT` (#1A4B91 - blue)
- Received messages: `BR_COLOR_RECV` (#21262D - dark surface)

### Status Indicators

- Radio OK: `BR_COLOR_SUCCESS` (#238636)
- Radio ERROR: `BR_COLOR_DANGER` (#DA3633)

## References

- Web app global styles: `webapp/src/styles/global.css`
- Firmware theme: `components/ui_graphics/theme/bramble_theme.h`
- UI documentation: `docs/ui-reference.md`
