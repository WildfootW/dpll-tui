# dpll-tui

ncurses TUI for inspecting and controlling Linux DPLL (Digital Phase-Locked Loop) devices via netlink / YNL.

## Screenshot (conceptual)

```
 dpll-tui  |  device 1/2: PPS  id=1  |  lock=locked  mode=automatic
 q quit | Tab device | Up/Down select | s state | p prio | a phase_adj | r refresh
────────────────────────────────────────────────────────────────────────────────────
 #    pin_id state          prio   phase_off(fs)  label
 0    5      selectable     7      -              CLK_78M125_NAC0_SYNCE0
 1    6      selectable     8      -              CLK_78M125_NAC0_SYNCE1
 2    7      connected      3      12345          GNSS_1PPS_IN
 3    8      disconnected   15     -              ETH01_SDP_TIMESYNC_0
```

## Features

- Auto-discovers PPS and EEC DPLL devices
- Live pin table: state, priority, phase offset, board/package label
- **Tab** to switch between devices
- **s** to set pin state via text menu (connected / disconnected / selectable)
- **p** to set pin priority
- **a** to set pin phase adjustment (femtoseconds)
- Auto-refresh every second; **r** for manual refresh

## Dependencies

| Dependency | Package (RHEL/Fedora) | Package (Debian/Ubuntu) |
|---|---|---|
| libynl headers + lib | `kernel-tools-libs-devel` | check `apt-file search ynl/ynl.h` |
| ncurses | `ncurses-devel` | `libncurses-dev` |
| gcc | `gcc` | `gcc` |

## Build

```bash
# Check deps interactively (optional)
chmod +x scripts/deps_check_fix_build.sh
./scripts/deps_check_fix_build.sh --no-build

# Build
make
```

Produces `./dpll-tui`.

## Usage

```bash
# Needs root for netlink DPLL access
sudo ./dpll-tui
```

### Key bindings

| Key | Action |
|---|---|
| `q` / `Esc` | Quit |
| `Tab` | Switch DPLL device (PPS / EEC) |
| `Up` / `Down` | Select pin |
| `s` | Set pin state (interactive menu) |
| `p` | Set pin priority (numeric input) |
| `a` | Set pin phase adjust in fs (numeric input) |
| `r` | Force refresh |

## Project structure

```
.
├── Makefile
├── README.md
├── VERSION
├── LICENSE
├── hdr/
│   ├── dpll_utils.h      # DPLL netlink wrapper API
│   └── log.h             # Logging macros
├── src/
│   ├── dpll-tui.c         # TUI (ncurses + main)
│   └── dpll_utils.c      # DPLL netlink operations (YNL)
└── scripts/
    └── deps_check_fix_build.sh
```

## License

MIT. See [LICENSE](LICENSE).
