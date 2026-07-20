# waybar-display

A waybar CFFI **settings** plugin for the asteroidz compositor — a tabbed popup:

- **Display** — per-monitor resolution/refresh, scale, VRR, HDR + luminance,
  and a draggable layout arranger (multi-monitor).
- **Wallpaper** — a thumbnail browser plus cycle settings: source folder, cycle
  interval, order (random/sequential), and shared-vs-per-monitor wallpapers.
  Reads/writes ~/.config/waybar/wallpaper.conf and applies via the
  set-wallpaper.sh / cycle-wallpaper.sh scripts (swaybg per-output + matugen).

Bar pill: monitor icon + current resolution·refresh. Click for a popup to set:

- **Resolution + refresh** — from the output's available modes
- **Scale** — fractional
- **VRR** (adaptive sync)
- **HDR** on/off — applied instantly (`amsg dispatch toggle_hdr`)
- **SDR reference luminance** — applied instantly (`set_sdr_luminance`)
- **HDR luminances** — max / min / max-frame-average (nits)

State is read from asteroidz IPC (`amsg get all-monitors` / `get monitor`,
which exposes an available-modes array). Resolution/scale/VRR and the HDR
luminances are written to a plugin-owned `~/.config/asteroidz/monitors.kdl`
(sourced by `config.kdl`) and applied with `reload_config`, which re-modesets
the live output. HDR toggle and SDR luminance apply live without a reload.

Uses [waybar-plugin-common](https://github.com/asteroidzman/waybar-plugin-common)
(git submodule at `common/`).

## Build & install

Arch Linux: `yay -S waybar-display` (AUR).

Requires `gtk3`, `glib2`, `json-glib` (and their dev headers), `asteroidz`
(driven by its IPC) and a C compiler.

```sh
git clone --recursive https://github.com/asteroidzman/waybar-display.git
cd waybar-display && make install
```
