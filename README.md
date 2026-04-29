# PNGRecorder v4.0 (AI Data Acquisition Engine)

High-performance, triple-threaded DirectX 11 capture engine designed for generating lossless training datasets for Computer Vision and AI models.

## 🚀 Engine Architecture
PNGRecorder v4.0 utilizes a high-speed pipeline optimized for high-end discrete GPUs and ultra-fast NVMe storage.

- **Zero-Allocation Buffer Pool:** 150-frame pre-allocated circular buffer (g_poolMemory) eliminates runtime heap churn and allocation lag.
- **RTX Persistent Mirror:** Captures via `AcquireNextFrame` into a `D3D11_USAGE_DEFAULT` texture, decoupling from the DXGI SwapChain immediately to prevent frame-drop during release.
- **Staging Texture Pinning:** Reuses a single `D3D11_USAGE_STAGING` buffer to keep GPU-to-CPU memory "warm," bypassing the overhead of 4K texture creation every frame.
- **Triple-Threaded Execution:**
    1. **Capture:** DXGI Desktop Duplication (GPU).
    2. **Transfer:** Resource Resolve, Flush, & Memcpy (GPU/CPU).
    3. **Writer:** Multi-threaded PNG encoding & NVMe I/O (CPU/Disk).

## 🛠 Dynamic Hardware Detection
Version 4.0 removes all hardcoded metadata. Every session performs deep-system interrogation to populate the `session.json`:

- **CPU:** Exact model string via Registry (`HARDWARE\DESCRIPTION\System\CentralProcessor\0`).
- **GPU:** Primary adapter enumeration via DXGI (Selecting the discrete RTX 4080 over IGPU).
- **RAM:** Total physical capacity via `GlobalMemoryStatusEx`.
- **Storage:** Physical bus-type identification (NVMe/SSD/HDD) via `IOCTL_STORAGE_QUERY_PROPERTY`.

## 📊 Telemetry & Metadata
The engine generates high-precision datasets ready for model ingestion:

- **metadata.csv:** 
  - `QPC Timestamp`: Microsecond-precision timing for temporal AI models.
  - `Normalized ROI`: Resolution-independent coordinates (0.0 - 1.0).
  - `Context Tracking`: Active Window Title tracking via `GetForegroundWindow`.
- **session.json:** Global context including hardware specs and session summary.

## 📂 Output Structure
```text
/Capture_Session_YYYYMMDD/
├── session.json       # Hardware specs, resolution, and session summary
├── metadata.csv       # Frame-by-frame telemetry (Timestamp, DeltaMS, ROI, Window Title)
└── [0000_capture].png # Lossless BGRA dataset frames
```

## 🛡 Reliability & Watchdogs
- **Atomic Drop Counter:** Tracks silent frame skips due to buffer pool exhaustion.
- **Black Frame Watchdog:** Validates GPU memory at Frame 5 to ensure the pipeline is "Resolve" ready.
- **HAGS Sync:** Uses `ID3D11DeviceContext::Flush()` to ensure pixel data is committed before `Map()` calls.
