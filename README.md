# RTL-SDR FM Radio

A real-time wideband FM demodulator written in C++20. This project interfaces with an RTL-SDR dongle to capture raw I/Q samples, processes the DSP pipeline (filtering, demodulation, de-emphasis), and plays audio via PortAudio or saves it to a WAV file.

## Features

* **Modern C++:** Built using C++20 standards.
* **DSP Pipeline:** Includes polyphase filtering, fast FM demodulation, I/Q & Audio DC blocking, De-emphasis, and Automatic Gain Control (AGC).
* **Dual Modes:**
    * **Live Streaming:** Real-time audio playback using PortAudio.
    * **Recording:** Captures 10 seconds of audio to `out.wav` by default.
* **Dependency Management:** fully integrated with **vcpkg** in manifest mode.

## Dependencies

The following libraries are handled automatically by `vcpkg`:
* **librtlsdr** (SDR hardware driver)
* **portaudio** (Audio I/O)

### Prerequisites

1.  **Hardware:** An RTL-SDR USB Dongle.
2.  **Drivers (Windows):** You must install the WinUSB driver for your dongle using **Zadig**.
3.  **Build Tools:**
    * Visual Studio 2019 or 2022 (with "Desktop development with C++" workload).
    * [CMake](https://cmake.org/download/).
    * [Vcpkg](https://github.com/microsoft/vcpkg).

## Building the Project

### Environment Setup
Ensure the `VCPKG_ROOT` environment variable is set in Windows to your vcpkg installation path.

### 1. VS Code Workflow (Recommended)
This project is configured with `CMakePresets.json` for seamless VS Code integration.

1.  Open the project folder in **VS Code**.
2.  Install the **CMake Tools** extension (Microsoft).
3.  The extension should detect the presets. In the **CMake Side Bar** (or Status Bar):
    * **Configure Preset:** Select `win-release` (Windows Release VS 2019).
    * **Build Preset:** Select `Release Build` (Essential for real-time audio performance).
4.  Press **F7** or click **Build**.

### 2. Command Line (CLI)
You can also build from a PowerShell terminal using CMake directly:

```powershell
# Configure (uses vcpkg toolchain automatically via preset)
cmake --preset win-release

# Build (Explicitly specify Release config)
cmake --build build/win-release --config Release