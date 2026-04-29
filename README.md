# PNGRecorder v4.0 (AI Data Acquisition Engine)

High-performance, triple-threaded DirectX 11 capture engine designed for generating lossless training datasets for Computer Vision and AI models.

## 🎯 Use Cases
This engine is built for researchers and developers who need high-fidelity visual data coupled with precise system telemetry:

- **Behavioral AI Training:** Capture user interactions with desktop applications to train agents on "human-like" navigation.
- **UI/UX Computer Vision:** Generate datasets for automated element detection, OCR, or layout analysis.
- **Latency & Performance Profiling:** Track exact frame-to-pixel delays for high-performance software testing.
- **Lossless Ground Truth:** Unlike video (H.264/H.265), PNGRecorder saves every pixel perfectly for sensitive model training that cannot tolerate compression artifacts.

## 🚀 Engine Architecture
- **Zero-Allocation Buffer Pool:** 150-frame pre-allocated circular buffer (g_poolMemory) eliminates runtime heap churn.
- **RTX Persistent Mirror:** Captures into a `D3D11_USAGE_DEFAULT` texture, decoupling from the DXGI SwapChain immediately.
- **Staging Texture Pinning:** Reuses a single staging buffer to keep GPU-to-CPU memory "warm," bypassing 4K allocation penalties.
- **Triple-Threaded Execution:** 
    - **Thread 1 (Capture):** DXGI Desktop Duplication.
    - **Thread 2 (Transfer):** Resource Resolve & Memcpy.
    - **Thread 3 (Writer):** Multi-threaded PNG encoding & NVMe I/O.

## 🛠 Installation & Setup

### Prerequisites
- **OS:** Windows 10/11 (DirectX 11.1+).
- **Hardware:** Discrete GPU (NVIDIA RTX 30/40 series recommended) and NVMe storage for 60FPS lossless recording.
- **Compiler:** MSVC (Visual Studio 2022) with C++17 or higher.

### Compilation
1. Clone the repository and open `.cpp` file in Visual Studio.
2. Link the following DirectX libraries in your Project Properties:
   - `d3d11.lib`, `dxgi.lib`
3. Ensure your `lodepng` or chosen PNG encoder source is included in the project.
4. Set the build configuration to **Release / x64** (Debug mode will likely fail to maintain 60FPS).

### Execution
1. Run the executable.
2. The engine will automatically interrogate your hardware (CPU/GPU/NVMe) and begin the session.
3. Check the **[LIVE]** console heartbeat for real-time FPS and buffer health.
4. Data is saved to a timestamped folder in the application directory.

## 📊 Telemetry & Metadata
- **metadata.csv:** Includes `QPC Timestamp`, `Normalized ROI` (0.0–1.0 for 4K space), and `Active Window Title`.
- **session.json:** Global context including dynamic hardware specs and capture summary.

## 📂 Output Structure
```text
/Capture_Session_YYYYMMDD/
├── session.json       # Hardware specs, resolution, and session summary
├── metadata.csv       # Frame-by-frame telemetry (Timestamp, DeltaMS, ROI, Window Title)
└── [0000_capture].png # Lossless BGRA dataset frames
```

## 🛡 Reliability & Watchdogs
- **Atomic Drop Counter:** Tracks frame skips due to buffer exhaustion.
- **Black Frame Watchdog:** Validates GPU memory at Frame 5.
- **HAGS Sync:** Uses `ID3D11DeviceContext::Flush()` to ensure pixel commitment.
