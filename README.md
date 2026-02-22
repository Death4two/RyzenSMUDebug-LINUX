# Ryzen SMU Debug Tool for Linux

A Linux port of the Windows ZenStates SMU Debug Tool. Communicates with the AMD System Management Unit (SMU) through the `ryzen_smu` kernel driver via `libsmu`, providing access to PM tables, SMU commands, SMN address space, and memory timings.

---

## ⚠️ WARNING — EXPERIMENTAL SOFTWARE

**This tool is experimental and may cause system instability, data loss, or hardware damage.**

- **Use at your own risk.** Low-level SMU and SMN access can misconfigure the CPU, motherboard, or firmware.
- **Hardware risk:** Incorrect or unsupported commands may contribute to permanent damage or "bricking" of hardware. The authors are not responsible for any damage.
- **PBO / Curve Optimizer / FMax tuning** in this tool is developed and tested **only for Granite Ridge (Zen 5 desktop)**. Other platforms are unsupported for tuning; behavior may be wrong or dangerous.
- Ensure you have backups and understand the risks before changing SMU settings.

---

## Credits and sources

This project builds on and references the following:

- **[ryzen_smu](https://github.com/amkillam/ryzen_smu)** — Linux kernel driver and userspace library (`libsmu`) for AMD Ryzen SMU/SMN access. Used for all communication with the SMU (PM table, RSMU/MP1/HSMP commands, SMN read/write).  
  - RSMU command reference: `ryzen_smu/docs/rsmu_commands.md`
- **ZenStates-Core** — Source for RSMU command IDs and PSM/Curve Optimizer behavior per platform (e.g. `GetDldoPsmMargin` / `SetDldoPsmMargin`, Zen3/Zen4/Zen5 SMUSettings). Core mask encoding and GET/SET command IDs (e.g. 0xD5 for Granite Ridge) are derived from ZenStates-Core.
- **SMUDebugTool (Windows)** — Windows reference implementation (ZenStates-based). Used for behavior parity: core mask encoding, PM table layout (index/offset/value), mailbox scan logic, and UI concepts for PBO/Curve Optimizer/FMax.

Thanks to the authors and contributors of the above projects.

## Prerequisites

- **Kernel driver**: The `ryzen_smu` kernel module must be loaded.
  ```bash
  cd ../ryzen_smu
  make
  sudo insmod ryzen_smu.ko
  ```
  Or install via DKMS for persistence across reboots.

- **Build tools**: `gcc`, `make`
- **Root access**: Required for driver communication

## Building

```bash
cd smu_debug_linux
make
```

The binary `smu_debug_tool` will be created in the current directory. If GTK3 is available (`pkg-config gtk+-3.0`), the build includes the GUI; run with `--gui` to use it.

Optional install to system path:
```bash
sudo make install   # installs to /usr/local/bin/
```

**GUI dependency:** For the graphical interface, install GTK3 development files, e.g.:
- Debian/Ubuntu: `sudo apt install libgtk-3-dev`
- Fedora: `sudo dnf install gtk3-devel`
- Arch: `sudo pacman -S gtk3`

## Usage

**CLI (interactive menu):**
```bash
sudo ./smu_debug_tool
```

**GUI:**
```bash
sudo ./smu_debug_tool --gui
```

The tool will auto-elevate via `sudo` if not run as root.

## GUI (--gui)

When built with GTK3, `smu_debug_tool --gui` opens a window with tabs:

| Tab | Contents |
|-----|----------|
| **System Info** | CPU model, codename, SMU version, topology, PM table version/size |
| **PM Table** | Full table of Index / Offset / Value / Max (same as CLI). Refresh or auto-refresh every 2 s |
| **PBO / Tuning** | **FMax override** (MHz): read/set. **Per-core Curve Optimizer**: cores 0–15 (range -60 to +10), **Read current CO**, per-core **Set**. *Granite Ridge only.* |
| **SMU Command** | Send arbitrary RSMU/MP1/HSMP command with 6 args (hex), view response |
| **SMN** | Read/write SMN address (hex) |
| **Log** | Status and error messages |

**Curve Optimizer** (Granite Ridge): per-core offset -60 to +10, Set PSM command 0x6, Get PSM 0xD5; core mask encoding matches ZenStates. **FMax**: GetMaxFrequency (0x6E), SetOverclockFreqAllCores (0x5C).

## CLI Features

| Option | Feature | Description |
|--------|---------|-------------|
| 1 | System Information | CPU name, codename, family/model, SMU FW version, PM table info, topology |
| 2 | Send SMU Command | Send arbitrary commands to RSMU/MP1/HSMP mailboxes with up to 6 args |
| 3 | PM Table Monitor | Live-updating display of all PM table float entries with max tracking |
| 4 | PM Table Dump | One-shot dump to stdout, CSV, or raw binary file |
| 5 | SMN Read | Read a single SMN address (shows HEX/DEC/BIN/FLOAT) |
| 6 | SMN Write | Write a value to an SMN address |
| 7 | SMN Range Scan | Dump a range of SMN addresses |
| 8 | SMU Mailbox Scan | Discover SMU mailbox CMD/RSP/ARG address triples |
| 9 | Memory Timings | Read DRAM timing parameters via SMN |
| A | Export JSON Report | Full system report with PM table snapshot |
| B | PM Table Summary | Named-field summary for known PM table versions (Matisse) |

### PM Table Monitor

The monitor mode displays all PM table entries as a paginated, live-updating table:

```
 Idx  |  Offset  |     Value      |      Max
──────┼──────────┼────────────────┼────────────────
 0000 │ 0x0000   │     142.000000 │     142.000000
 0001 │ 0x0004   │      88.234100 │      95.123400
 ...
```

- **Index**: Sequential position in the float array (matches Windows tool's `{i:D4}`)
- **Offset**: Byte offset (`index * 4`, matches Windows tool's `0x{i*4:X4}`)
- **Value**: Current IEEE 754 float value (6 decimal places)
- **Max**: Highest value seen since monitor start

Controls: `[n]`ext page, `[p]`rev page, `[r]`eset max, `[q]`uit

### SMU Mailbox Scan

Replicates the Windows tool's `ScanSmuRange` logic:
1. Writes unknown command (0xFF) to potential CMD addresses
2. Checks for UnknownCmd (0xFE) response at expected RSP offsets
3. Validates with GetSMUVersion (0x02) command
4. Discovers ARG address by scanning for the version value
5. Verifies with TestMessage (0x01) expecting `arg + 1` response

Scan ranges are codename-dependent, matching the Windows tool exactly.

## Curve Optimizer and FMax (CLI and GUI)

- **FMax:** Read with RSMU command `0x6E` (GetMaxFrequency), set with `0x5C` (SetOverclockFreqAllCores, frequency in MHz). Supported on Matisse/Vermeer and similar; other platforms may use different commands.
- **Curve Optimizer:** Per-core margin is sent via RSMU command `0x76` (Set PSM margin) with Arg0 = core mask, Arg1 = signed margin. If your CPU does not respond to `0x76`, the feature may be unsupported or use a different command ID on your platform; use the SMU Command tab/page to experiment.

## Supported platforms

**PBO / Curve Optimizer / FMax tuning (GUI and related CLI):**  
**Only Granite Ridge (Zen 5 desktop, e.g. Ryzen 9000 series)** is supported and tested. Other CPUs may be detected and basic SMU/PM table features may work, but tuning (CO, FMax) is unsupported and may be incorrect or unsafe.

**General SMU/PM table/SMN (CLI, other tabs):**  
Behavior depends on the `ryzen_smu` driver. Many AMD Ryzen processors are supported by the driver (e.g. Matisse, Vermeer, Raphael, Granite Ridge, Renoir, Cezanne, Phoenix, Milan, and others). See the driver repository for the full list.

## License

GPL-3.0 - See source files for details.
