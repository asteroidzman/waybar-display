# waybar-display

A waybar CFFI plugin to configure the asteroidz compositor's active monitor.

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

```sh
git clone --recursive https://github.com/asteroidzman/waybar-display.git
cd waybar-display && make install
```
