# PNGRecorder v3.6 🚀

**High-Performance Lossless Screen Capture for AI Training Datasets**

PNGRecorder is a C++ Windows utility designed for high-fidelity data acquisition. Unlike standard screen recorders that use lossy video compression (H.264/H.265), PNGRecorder captures the desktop as a sequence of raw, 24-bit RGB PNGs. This ensures every pixel is mathematically perfect, making it ideal for training Computer Vision and multimodal AI models.

## 🌟 Key Features
*   **60+ FPS Performance:** Optimized for high-end hardware (i9-11900K, NVMe SSDs).
*   **Zero-Allocation Buffer Pool:** Custom "Shock Absorber" memory pool eliminates heap jitter and 15.6ms Windows scheduling delays.
*   **Lossless Fidelity:** Saves frames directly as PNGs to avoid compression artifacts.
*   **Flexible ROI Selection:** Manual mouse selection, configuration loading, or coordinate-based overrides.
*   **Automated Metadata:** Generates a `metadata.csv` with high-precision QPC timestamps for every frame.

## 🏗️ Architecture Overview
The system utilizes a **Triple-Threaded Pipeline** to maximize throughput:
1.  **Capture Thread:** Uses DirectX 11 Desktop Duplication API to grab GPU textures and copies them to the pre-allocated CPU buffer pool.
2.  **Processing Thread:** Handles BGRA to RGB conversion and cropping using optimized pointer-step arithmetic.
3.  **IO Worker:** Encodes and writes PNGs to disk using the Windows Imaging Component (WIC) API.

## 🚀 Getting Started

### Requirements
*   **OS:** Windows 10/11 (64-bit)
*   **Hardware:** Optimized for high-speed NVMe storage and 32GB+ RAM.
*   **Compiler:** Visual Studio 2022 (MSVC)

### Installation
1. Clone the repository:
   ```powershell
   git clone https://github.com/princeeriktheblue/PNGRecorder
   ```
2. Open the `.sln` file in Visual Studio.
3. Build in **Release x64** mode.

## 🛠️ Usage
PNGRecorder supports several execution modes to fit different training workflows:

*   **Quick Start (Manual Selection):**
    ```powershell
    .\PNGRecorder.exe
    ```
*   **Config window (Save to `roi.cfg`):**
    ```powershell
    .\PNGRecorder.exe -c
    ```
*   **Headless Capture (Load from `roi.cfg`):**
    ```powershell
    .\PNGRecorder.exe -l [OutputDir] [DurationSec] [IntervalSec]
    ```
*   **60 FPS Stress Test:**
    ```powershell
    .\PNGRecorder.exe -m . 10 0.016 0
    ```
*   **Command Line ROI Override:**
    ```powershell
    .\PNGRecorder.exe -r [OutputDir] [Duration] [Interval] [MonitorIndex] [X Y W H]
    ```

## 📊 Technical Benchmarks

| Metric | v3.5 | v3.6 (Current) |
| :--- | :--- | :--- |
| **Max FPS** | ~15-20 FPS | **62.5+ FPS** |
| **Timing Resolution** | 15.6ms (System Clock) | **<1ms (QPC)** |
| **Memory Management** | Heap Allocation | **Pre-allocated Pool** |

## 📁 Output Structure
Every session is saved into a unique timestamped folder:
```text
Session_YYYYMMDD_HHMMSS/
├── 0001_capture.png
├── 0002_capture.png
└── metadata.csv
```

---
**Note:** This tool is designed for high-fidelity data acquisition and can consume significant disk space quickly. Use with high-capacity NVMe storage.
