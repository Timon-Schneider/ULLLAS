# ULLLAS — Ultra Low Latency LAN Audio Streamer

Point-to-multipoint uncompressed audio streaming over LAN with ASIO (Windows), CoreAudio (macOS), and JACK (Linux) backends. Designed for the lowest possible latency. Stream your microphone or game audio from one PC to another, or broadcast to multiple receivers.

## Features

- **ASIO backend** (Windows) — bypasses the Windows audio stack entirely, ~1–3ms capture/playback latency
- **CoreAudio backend** (macOS) — native AudioUnit HAL backend, full send/receive support
- **JACK backend** (Linux) — connects to the JACK graph for pro-audio routing
- **Raw PCM over UDP multicast** — no codec latency, single packet reaches all receivers
- **Configurable bit depth** — 16/24/32-bit, user-selectable per `--bit-depth`
- **Channel routing** — map specific ASIO/JACK inputs/outputs to network streams
- **Jitter buffer** — configurable 1–8 packet buffer for network jitter smoothing
- **Zero external runtime dependencies** — fully statically linked on Windows, single `.exe`; uses system frameworks on macOS/Linux

---

## Building on Windows

### 1. Install MSYS2

Download the installer from [msys2.org](https://www.msys2.org/) and run it.  
**Install to the default location** (`C:\msys64`).

After installation, a terminal opens. Run these commands to update and install the toolchain:

```bash
pacman -Syu
# (terminal will close — reopen it manually from Start Menu → MSYS2 → UCRT64)
pacman -Su
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-make
```

> **Why UCRT64?** The UCRT (Universal C Runtime) environment produces binaries that run on any Windows 10+ machine without extra DLLs. The older MINGW64 environment depends on `msvcrt.dll` which ships with Windows but has worse C99/C11 support.

Verify the tools are installed:

```bash
gcc --version    # should show 14.x or later
g++ --version    # same
cmake --version  # 3.15 or later
```

> **Important:** Always use the **UCRT64** terminal for building. Do NOT use the MSYS2 terminal — it targets a different runtime.

### 2. ASIO SDK

The Steinberg ASIO SDK is **already included** in the `asio/` directory. The SDK is available under GPLv3 from [steinberg.net/developers/asiosdk-open/](https://www.steinberg.net/developers/asiosdk-open/).

No manual download is needed — just build. The `asio/` directory contains:

```
asio/
├── asio.h
├── asio.cpp
├── asiodrivers.h
├── asiodrivers.cpp
├── asiolist.h
├── asiolist.cpp
├── asiosys.h
├── ginclude.h
└── iasiodrv.h
```

If CMake doesn't find `asio.h`, it prints a warning and builds without ASIO support (the program won't work on Windows without it).

> **License note:** The ASIO SDK is copyright (c) Steinberg Media Technologies GmbH, licensed under GPLv3. See the LICENSE file for details. ASIO is a trademark of Steinberg Media Technologies GmbH.

### 3. Build

Open the **UCRT64** terminal, navigate to the project directory, and run:

```bash
mkdir build
cd build
cmake .. -G "MinGW Makefiles"
cmake --build .
```

Or as a one-liner from PowerShell (adjust paths):

```powershell
& "C:\Program Files\CMake\bin\cmake.exe" -G "MinGW Makefiles" -S "C:\Users\...\ULLLAS" -B "C:\Users\...\ULLLAS\build"
& "C:\Program Files\CMake\bin\cmake.exe" --build "C:\Users\...\ULLLAS\build"
```

The output binary is `build/ulllas.exe`. It is fully statically linked — no MSYS2 DLLs required on the target machine. Copy the single `.exe` to any Windows 10+ PC.

> **Note:** CMake auto-detects GCC from the UCRT64 environment. If you have Visual Studio installed, explicitly use `-G "MinGW Makefiles"` to force GCC. You also need `mingw32-make` in your PATH (it comes with the MSYS2 GCC package).

### 4. Verify

```bash
./ulllas.exe send --list-devices
```

Should print available ASIO drivers (ASIO4ALL v2, Focusrite USB ASIO, RME, etc.).

---

## Building on Linux

Tested on Debian 12 with JACK 1.9.21.

### Prerequisites

```bash
# Debian/Ubuntu
sudo apt install build-essential cmake pkg-config libjack-jackd2-dev

# Fedora
sudo dnf install gcc-c++ cmake pkg-config jack-audio-connection-kit-devel

# Arch
sudo pacman -S base-devel cmake jack2
```

### Build

Build from the project root (not inside `build/`):

```bash
rm -rf build && mkdir build
cmake -S . -B build
cmake --build build -j$(nproc)
```

The binary is `build/ulllas`.

If you make changes to the source and want to rebuild incrementally:

```bash
cmake --build build -j$(nproc)
```

### Verify

```bash
./build/ulllas --help
./build/ulllas send --backend jack --list-devices
```

### Running

Start JACK (e.g., via `qjackctl`), then:

```bash
# Sender: capture from system inputs
./build/ulllas send --backend jack --in-channels 0,1 --target 239.77.77.77:9000

# Receiver: play to system outputs
./build/ulllas recv --backend jack --out-channels 0,1 --port 9000
```

You can also connect ports manually with `jack_connect` instead of auto-connect.

> **Note:** The `--sample-rate` and `--buffer` options are ignored when using JACK — the JACK server controls sample rate and buffer size. Configure these in your JACK server settings (e.g., `qjackctl`).

---

## Building on macOS (Intel / Apple Silicon)

Uses the native CoreAudio framework — no additional dependencies required.

### Prerequisites

```bash
# Install Xcode Command Line Tools (provides clang, make, system headers)
xcode-select --install

# Option A: Homebrew (recommended, adds cmake to PATH)
brew install cmake

# Option B: Download the GUI app from https://cmake.org/download/
#           Installs to /Applications/CMake.app
```

> **Note:** The project uses CMake, not Xcode. The Command Line Tools alone are sufficient — the full Xcode.app is not required.

### Build

Build from the project root (the directory containing `CMakeLists.txt`):

```bash
git clone <repo-url> && cd ULLLAS
```

Then run configure and build. **Use Option A or B depending on how you installed CMake:**

**Option A — Homebrew cmake (in PATH):**
```bash
cmake -S . -B build
cmake --build build -j$(sysctl -n hw.ncpu)
```

**Option B — CMake GUI app (use full path):**
```bash
/Applications/CMake.app/Contents/bin/cmake -S . -B build
/Applications/CMake.app/Contents/bin/cmake --build build -j$(sysctl -n hw.ncpu)
```

The binary is built as an app bundle at `build/ulllas.app/Contents/MacOS/ulllas`.

If you make changes to the source and want to rebuild incrementally:

**Homebrew cmake:**
```bash
cmake --build build -j$(sysctl -n hw.ncpu)
```

**CMake GUI app:**
```bash
/Applications/CMake.app/Contents/bin/cmake --build build -j$(sysctl -n hw.ncpu)
```

### Verify

```bash
# List audio devices
./build/ulllas.app/Contents/MacOS/ulllas send --list-devices

# Send mic audio to the network
./build/ulllas.app/Contents/MacOS/ulllas send --in-channels 0,1

# Receive from a Windows/Linux sender on the same LAN
./build/ulllas.app/Contents/MacOS/ulllas recv --out-channels 0,1
```

> **Note:** When running sender mode for the first time, macOS will prompt for microphone permission. Approve it in **System Settings > Privacy & Security > Microphone**.
>
> **Note:** On macOS `--buffer` requests a specific I/O buffer size from CoreAudio. The system may round the value to the nearest supported size. You'll see a warning if the actual buffer differs from the requested value.

### Sample session

```bash
# On a Windows or Linux PC (sender):
ulllas send --in-channels 0,1 --target 239.77.77.77

# On the Mac (receiver):
./build/ulllas.app/Contents/MacOS/ulllas recv --out-channels 0,1
```

Or from macOS as sender:

```bash
# On the Mac (sender):
./build/ulllas.app/Contents/MacOS/ulllas send --in-channels 0,1

# On a Windows or Linux PC (receiver):
ulllas recv --out-channels 0,1
```

---

## Usage

### List available audio devices

```bash
ulllas send --list-devices
ulllas send --backend jack --list-devices
```

### Sender (capture audio and stream to network)

```bash
# Default multicast (group 239.77.77.77, port 9000) — sender and receiver auto-config
ulllas send --in-channels 0,1
ulllas recv --out-channels 0,1

# Custom multicast group
ulllas send --in-channels 0,1 --target 239.77.77.77
ulllas recv --out-channels 0,1 --target 239.77.77.77

# Custom port (both sides must match)
ulllas send --in-channels 0,1 --port 9001
ulllas recv --out-channels 0,1 --port 9001

# Single channel (mono), specific device, lower latency
ulllas send --device "Focusrite" --in-channels 0 --buffer 64

# Unicast mode (point-to-point over specific interface)
ulllas send --in-channels 0,1 --target 192.168.1.100 --unicast --iface 192.168.1.10
```

### Receiver (receive network audio and play locally)

```bash
# Default multicast receive (auto-joins group 239.77.77.77, port 9000)
ulllas recv --out-channels 0,1

# Custom multicast group — must match the sender's --target
ulllas recv --out-channels 0,1 --target 239.77.77.77

# Custom channel routing: network stream ch0 → speaker ch4, stream ch1 → speaker ch5
ulllas recv --out-channels 4,5

# More jitter buffer for unreliable networks
ulllas recv --out-channels 0,1 --jitter 4
```

### Multi-receiver (broadcast)

Just run the receiver on multiple PCs — the sender uses multicast by default, so one send reaches all receivers. Both sides default to group 239.77.77.77 port 9000.

```
PC1: ulllas send --in-channels 0,1
PC2: ulllas recv --out-channels 0,1
PC3: ulllas recv --out-channels 0,1
```

### Full-duplex (send and receive simultaneously)

Run two instances:

```bash
# PC1: send mic, receive remote audio
ulllas send --in-channels 0,1 --target 239.77.77.77 --port 9000
ulllas recv --out-channels 0,1 --port 9001

# PC2: send mic, receive remote audio
ulllas send --in-channels 0,1 --target 239.77.77.77 --port 9001
ulllas recv --out-channels 0,1 --port 9000
```

### All options

| Flag | Mode | Default | Description |
|---|---|---|---|---|
| `--backend <name>` | both | auto | Audio backend: `coreaudio` (macOS), `asio` (Win), `jack` (Linux) |
| `--device <name>` | both | auto | Audio device name substring match |
| `--in-channels <a,b,…>` | send | `0,1` | ASIO/JACK/CoreAudio input channel indices to capture |
| `--out-channels <a,b,…>` | recv | `0,1` | ASIO/JACK/CoreAudio output channel indices to play to |
| `--sample-rate <hz>` | both | `48000` | Sample rate (8000–384000). Ignored on JACK — server sets rate |
| `--bit-depth <bits>` | both | `24` | Network payload bit depth: `16`, `24`, or `32` |
| `--buffer <samples>` | both | `128` | Audio buffer size (8–4096). Lower = less latency. On macOS the device may round to nearest supported size |
| `--target <ip>` | both | `239.77.77.77` | Target IP (sender) or multicast group to join (receiver) |
| `--bind <ip>` | recv | `0.0.0.0` | IP to bind the receiving socket to |
| `--port <port>` | both | `9000` | UDP port to send to / listen on |
| `--iface <ip>` | send | `0.0.0.0` | Outbound interface IP for multicast |
| `--unicast` | both | off | Use unicast instead of multicast |
| `--jitter <packets>` | recv | `2` | Jitter buffer size in packets (1–8) |
| `--list-devices` | both | — | List audio devices and exit |
| `--verbose` | both | off | Show peak levels in status line |
| `--help` / `-h` | both | — | Print usage |

---

## ASIO4ALL Configuration

ASIO4ALL v2 wraps WDM (Windows) audio devices into an ASIO interface. When ULLLAS is running, an ASIO4ALL icon appears in the system tray.

**Critical step:** Click the tray icon to open the control panel. Enable your input/output devices by clicking the **blue power button** next to each device. Without this, ASIO4ALL routes audio to nowhere — you'll see peak levels but hear nothing.

For streaming game audio (speaker output, not microphone), you need a loopback device:
- [VB-Cable](https://vb-audio.com/Cable/) (free virtual audio cable)
- Enable "Stereo Mix" in your sound card settings (if supported)
- Or use a hardware loopback cable

Select the loopback device in ASIO4ALL as the input source on the sender PC.

---

## Troubleshooting

### "ASIO: Failed to load driver"
- The ASIO driver DLL is not registered or not found. Reinstall your ASIO driver.
- ASIO4ALL v2 must be installed. Download from [asio4all.org](https://asio4all.org/).

### No audio on receiver (but peak levels show)
- Click the ASIO4ALL tray icon on the receiver PC and enable the speaker output (blue power button).
- Verify the output device is not muted in Windows sound settings.

### "missing libgcc_s_seh-1.dll" or "missing libstdc++-6.dll" or "missing libwinpthread-1.dll"
- You're running a build from before static linking was enabled. Rebuild with the current CMakeLists.txt (it uses `-static` on Windows).
- Or install the MSYS2 UCRT64 runtime on the target machine (not recommended — use the static build).

### High latency / audio glitches
- Reduce `--buffer` (try 64 or 96) for lower latency. On macOS, check if the device rounded your request.
- Increase `--jitter` (1–8) on the receiver for fewer glitches on unreliable networks.
- Use a wired Ethernet connection. Wi-Fi adds unpredictable jitter.
- Close other applications using the audio driver.
- On Linux, reduce the JACK server's buffer size via `qjackctl` or `jackd` settings.

---
