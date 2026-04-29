# 📸 PNGRecorder v4.0 (AI Data Acquisition Engine)

A high-performance, triple-threaded DirectX 11.1 engine designed for high-throughput, lossless dataset generation. Engineered specifically for **NVIDIA RTX 40-series** hardware and **NVMe storage** to capture 60+ FPS without compression artifacts.

---

## 🚀 Technical Architecture

PNGRecorder v4.0 treats screen capture as a **data acquisition** task rather than a video task, eliminating bottlenecks common in consumer recording software.

*   **Triple-Threaded Pipeline:** 
    1.  **Capture (Main):** QPC-timed DXGI Desktop Duplication with a `persistentCopy` GPU mirror.
    2.  **Processor Thread:** Swizzles BGRA to 24bpp RGB and handles ROI cropping.
    3.  **I/O Thread (WIC):** High-speed PNG encoding via Windows Imaging Component (WIC).
*   **Zero-Allocation Buffer Pool:** A 150-frame pre-allocated `VirtualAlloc` block prevents heap fragmentation and OS-level memory latency.
*   **GPU Sync Fix:** Implements `ID3D11DeviceContext::Flush()` and persistent staging textures to eliminate "black frame" issues common on discrete GPUs under high load.

---

## 🛠 Dynamic Hardware Detection

The engine performs deep-system interrogation to ensure your AI training metadata is complete and reproducible:
*   **CPU:** Exact marketing string via Registry (`HARDWARE\DESCRIPTION\System\CentralProcessor\0`).
*   **GPU:** Active adapter enumeration via DXGI (identifies specific hardware like RTX 4080).
*   **RAM:** Total physical capacity via `GlobalMemoryStatusEx`.
*   **Storage:** **IOCTL** bus-type queries to verify **NVMe** vs. SSD/HDD throughput capabilities.

---

## ⌨️ Command Line Usage

PNGRecorder supports four distinct modes. Arguments are processed **positionally**.

```bash
PNGRecorder.exe [mode] [out_dir] [duration] [interval] [monitor_idx] [x] [y] [w] [h]
```

### Argument Definitions


| Index | Argument | Example | Description |
| :--- | :--- | :--- | :--- |
| 1 | **Mode** | `-r` | `-c`: Save ROI to config \| `-l`: Load from config \| `-m`: Manual Selector \| `-r`: Raw CLI |
| 2 | **Out Dir** | `C:\Data` | The root directory where timestamped session folders are created. |
| 3 | **Duration** | `60` | Recording length in **seconds**. |
| 4 | **Interval** | `0.016` | Sampling rate in **seconds** (`0.016` = 60 FPS, `0.033` = 30 FPS). |
| 5 | **Monitor** | `0` | Index of the monitor (0 = Primary). |
| 6-9 | **X, Y, W, H** | `0 0 1920 1080` | Pixel coordinates (Required ONLY for `-r` mode). |

---

### 💡 Example Commands

**1. Manual UI Selection (Default)**
Run with no arguments to launch the transparent ROI selector. After dragging your region, it records for 10s at 2 FPS by default.
```bash
PNGRecorder.exe
```

**2. Headless 60 FPS 4K Capture**
Record the full 4K screen for 1 minute directly to an external NVMe drive.
```bash
PNGRecorder.exe -r E:\Captures 60 0.0166 0 0 0 3840 2160
```

**3. Configure ROI for Automated Jobs**
Use the selector once to save a region to `roi.cfg`, then reload it for future sessions.
```bash
PNGRecorder.exe -c              # Select and save to local roi.cfg
PNGRecorder.exe -l C:\Data 300  # Load config and record for 5 mins
```

---

## 📊 Telemetry & Output Structure

Every session generates a folder named `Session_YYYYMMDD_HHMMSS/`:

*   **`session.json`**: Contains the Hardware profile (CPU/GPU/Storage) and a final session summary including total frames captured, dropped frames, actual average FPS, and duration.
*   **`metadata.csv`**: Frame-by-frame stats including `QPC Timestamp`, `DeltaMS`, `Normalized ROI Coordinates`, and the `Active Window Title`.
*   **`[index]_capture.png`**: Raw, lossless 24bpp RGB pixel data.

---

## 🛡 Reliability & Watchdogs

*   **Atomic Drop Counter:** Real-time tracking of skipped frames (`g_droppedFrames`) if the buffer pool exhausts.
*   **Black Frame Watchdog:** Scans pixel data every 30 frames to detect GPU driver stalls or memory zeroing.
*   **Timing Spike Detection:** Console alerts if a frame's `DeltaMS` exceeds 2x the target interval.
*   **Safe Shutdown:** Graceful thread joining and resource release (`VirtualFree`, `Release`) to ensure no data loss or memory leaks.

---

## 🏗 Build Requirements
*   **OS:** Windows 10/11 (DirectX 11.1+)
*   **Compiler:** MSVC (Visual Studio 2022) / C++17
*   **Libraries:** `d3d11.lib`, `dxgi.lib`, `windowscodecs.lib`, `shlwapi.lib`, `shell32.lib`, `winmm.lib`, `advapi32.lib`

**Project Lead:** AI Data Acquisition Team  
**Status:** v4.0 STABLE
