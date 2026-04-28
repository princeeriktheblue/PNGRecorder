# AI Viewport Capture Tool (v3.4)

A high-performance C++ utility designed for capturing lossless, time-indexed visual data from Windows viewports. Optimized for feeding visual data to AI models, debugging game states, or creating high-fidelity datasets.

## 🚀 Key Features

- **Triple-Buffered Architecture**: Decouples GPU acquisition, pixel processing, and I/O. Uses system RAM as a shock absorber to ensure zero frame-skipping on high-speed NVMe drives.
- **Interactive ROI Selector**: A Dim-and-Drag UI at startup allows you to visually define a static Region of Interest (ROI) (e.g., a game viewport or a specific HUD element).
- **AI-Native Output**: 
    - Converts Windows native **BGRA** to standard **24-bit RGB** on the fly.
    - Strips Alpha channels to reduce input tensor complexity.
- **Session-Based Organization**: Automatically generates unique, timestamped folders for every burst.
- **Metadata Logging**: Outputs a `metadata.csv` for every session, mapping frame indices to filenames and exact millisecond timestamps.
- **Sub-Second Precision**: Supports float-based intervals (e.g., `0.1` for 10 FPS capture).

## 🛠 Prerequisites

- **OS**: Windows 10 or 11.
- **Hardware**: DirectX 11 compatible GPU and a fast SSD (NVMe recommended).
- **RAM**: Optimized for high-capacity setups (e.g., 64GB+).
- **Development**: Visual Studio 2019/2022 with the "Desktop development with C++" workload.

## 📦 Setup & Compilation

1. Create a new **C++ Console App** in Visual Studio.
2. Replace the contents of `main.cpp` with the provided source code.
3. Set the Solution Platform to **x64**.
4. Build in **Release** mode for maximum performance.

The project automatically links required libraries via pragmas:
`d3d11.lib`, `dxgi.lib`, `windowscodecs.lib`, `shlwapi.lib`, `shell32.lib`.

## 📖 Usage

Run the executable via Command Prompt or PowerShell:

```bash
.\PNGRecorder.exe [Base_Path] [Duration_Sec] [Interval_Sec] [Monitor_Index]
```

### Examples:
- **Standard Debug**: `.\PNGRecorder.exe C:\DebugData 30 1.0 0`  
  *(30 seconds, 1 frame/sec, Primary Monitor)*
- **High-Speed Burst**: `.\PNGRecorder.exe D:\AI_Training 10 0.1 0`  
  *(10 seconds, 10 frames/sec, Primary Monitor)*

### Operation:
1. Launch the tool. The screen will dim.
2. **Click and drag** to draw a red box around your target viewport.
3. Release to start the capture session.
4. Press **ESC** during selection to default to Full Screen.

## 📂 Output Structure

```text
Target_Directory/
└── Session_20240325_143005/
    ├── metadata.csv
    ├── 0001_debug_20240325_143006.png
    ├── 0002_debug_20240325_143007.png
    └── ...
```

### Metadata Format:


| FrameIndex | FileName | TimestampMS |
| :--- | :--- | :--- |
| 1 | 0001_debug_...png | 102 |
| 2 | 0002_debug_...png | 1105 |

## ⚖️ License
This project is provided "as-is" for developer and AI research purposes.

## 🔧 Troubleshooting

### 🛑 Incorrect Colors (Red/Blue Swap)
Windows natively captures frames in **BGRA** (Blue-Green-Red-Alpha) order. While this tool includes an internal swapper to output standard **RGB**, some specific "Studio" environments or GPU drivers may provide a different raw buffer.

**If your blues appear orange or vice-versa:**
1. Locate the `ProcessingWorker` function in the source code.
2. Find the nested `for` loops where pixels are copied.
3. Toggle the indices `0` and `2` in the mapping block:

```cpp
// Current (RGB)
dRow[x * 3 + 0] = sR[x * 4 + 0]; // R
dRow[x * 3 + 2] = sR[x * 4 + 2]; // B

// Change to this if colors are swapped:
dRow[x * 3 + 0] = sR[x * 4 + 2]; 
dRow[x * 3 + 2] = sR[x * 4 + 0];
```

### 🛑 Error: "Could not duplicate output"
This usually occurs for one of two reasons:
1. **Conflicting Software**: Only one application can use the Desktop Duplication API at a time per monitor. Close other screen recorders or overlays (like OBS or Discord Stream).
2. **Hybrid Graphics**: On laptops, ensure the tool is forced to run on the **High-Performance GPU** (NVIDIA/AMD) via *Windows Settings > Graphics Settings*, matching the GPU that powers your display.

### 🛑 Missing DLLs
Ensure you have the **Windows 10/11 SDK** installed via the Visual Studio Installer. The tool relies on `D3D11.dll`, `DXGI.dll`, and `Windowscodecs.dll`, which are standard system files.

