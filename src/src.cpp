#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <timeapi.h>
#include <iomanip>
#include <atomic>
#include <winioctl.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "advapi32.lib")

// --- Data Structures ---
struct RawFrame {
    BYTE* bgraData = nullptr;
    UINT width = 0;
    UINT height = 0;
    UINT rowPitch = 0;
    int tx = 0;
    int ty = 0;
    int tw = 0;
    int th = 0;
    std::wstring fileName;
    double timestampMS = 0;
    int frameIndex = 0;
    int poolIndex = -1;
};

struct SaveTask {
    std::vector<BYTE> rgbData;
    UINT width = 0;
    UINT height = 0;
    UINT rowPitch = 0;
    std::wstring filePath;
};

struct ROI {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

// --- Memory Pool & Threading ---
const int POOL_SIZE = 150;
BYTE* g_poolMemory = nullptr;
std::queue<int> g_emptyPool;
std::mutex g_poolMutex;

ROI g_finalROI;
std::queue<RawFrame> g_rawQueue;
std::mutex g_rawMutex;
std::condition_variable g_rawCV;

std::queue<SaveTask> g_saveQueue;
std::mutex g_saveMutex;
std::condition_variable g_saveCV;

bool g_running = true;
std::wstring g_outputDir;
POINT g_ptStartSel = { 0, 0 };
bool g_isDrawingSel = false;
std::atomic<int> g_droppedFrames{ 0 };
std::atomic<int> g_ioFailures{ 0 };

// --- Helpers ---
double GetQPCMS() {
    static LARGE_INTEGER freq;
    static bool init = QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart * 1000.0 / (double)freq.QuadPart;
}

std::wstring GetExeDir() {
    wchar_t ep[MAX_PATH];
    GetModuleFileNameW(NULL, ep, MAX_PATH);
    std::wstring s(ep);
    return s.substr(0, s.rfind(L'\\') + 1);
}

std::wstring GetActiveWindowTitle() {
    wchar_t title[256];
    HWND handle = GetForegroundWindow();
    if (handle) {
        if (GetWindowTextW(handle, title, 256) > 0)
            return std::wstring(title);
    }
    return L"Unknown";
}

// --- Hardware Detection ---
std::wstring GetCPUName() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return L"Unknown CPU";
    wchar_t buf[256] = {};
    DWORD size = sizeof(buf);
    RegQueryValueExW(hKey, L"ProcessorNameString", nullptr, nullptr, (LPBYTE)buf, &size);
    RegCloseKey(hKey);
    std::wstring s(buf);
    size_t a = s.find_first_not_of(L' ');
    size_t b = s.find_last_not_of(L' ');
    return (a == std::wstring::npos) ? L"Unknown CPU" : s.substr(a, b - a + 1);
}

std::wstring GetRAMSummary() {
    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);
    ULONGLONG gb = (ms.ullTotalPhys + (512ULL << 20)) >> 30;
    return std::to_wstring(gb) + L"GB RAM";
}

std::wstring GetGPUNameDXGI() {
    IDXGIFactory* fac = nullptr;
    if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&fac)))
        return L"Unknown GPU";
    IDXGIAdapter* adp = nullptr;
    std::wstring name = L"Unknown GPU";
    if (SUCCEEDED(fac->EnumAdapters(0, &adp))) {
        DXGI_ADAPTER_DESC desc;
        if (SUCCEEDED(adp->GetDesc(&desc)))
            name = desc.Description;
        adp->Release();
    }
    fac->Release();
    return name;
}

static bool QueryDriveBusType(wchar_t letter, bool& nvme, bool& ssd) {
    wchar_t volPath[16];
    swprintf_s(volPath, L"\\\\.\\%c:", letter);
    HANDLE hVol = CreateFileW(volPath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hVol == INVALID_HANDLE_VALUE)
        return false;
    VOLUME_DISK_EXTENTS vde = {};
    DWORD ret = 0;
    int diskNum = -1;
    if (DeviceIoControl(hVol, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, &vde, sizeof(vde), &ret, nullptr))
        diskNum = (int)vde.Extents[0].DiskNumber;
    CloseHandle(hVol);

    wchar_t physPath[32];
    if (diskNum >= 0)
        swprintf_s(physPath, L"\\\\.\\PhysicalDrive%d", diskNum);
    else
        swprintf_s(physPath, L"\\\\.\\%c:", letter);

    HANDLE h = CreateFileW(physPath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    STORAGE_PROPERTY_QUERY q = {};
    q.QueryType = PropertyStandardQuery;
    ret = 0;
    char buf[512] = {};

    q.PropertyId = StorageDeviceProperty;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf), &ret, nullptr)) {
        auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buf);
        nvme = (desc->BusType == BusTypeNvme);
        if (!nvme) {
            std::string raw(buf, ret);
            for (auto& c : raw)
                c = (char)tolower((unsigned char)c);
            if (raw.find("nvme") != std::string::npos)
                nvme = true;
        }
    }

    DEVICE_SEEK_PENALTY_DESCRIPTOR spd = {};
    q.PropertyId = StorageDeviceSeekPenaltyProperty;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), &spd, sizeof(spd), &ret, nullptr))
        ssd = !spd.IncursSeekPenalty;

    CloseHandle(h);
    return true;
}

std::wstring GetStorageSummary() {
    ULONGLONG nvmeBytes = 0;
    ULONGLONG ssdBytes = 0;
    ULONGLONG hddBytes = 0;
    wchar_t drv[512] = {};
    GetLogicalDriveStringsW(512, drv);
    for (wchar_t* p = drv; *p; p += wcslen(p) + 1) {
        if (GetDriveType(p) != DRIVE_FIXED)
            continue;
        ULARGE_INTEGER total = {};
        if (!GetDiskFreeSpaceExW(p, nullptr, &total, nullptr))
            continue;
        bool isNVMe = false;
        bool isSSD = false;
        QueryDriveBusType(p[0], isNVMe, isSSD);
        if (isNVMe)
            nvmeBytes += total.QuadPart;
        else if (isSSD)
            ssdBytes += total.QuadPart;
        else
            hddBytes += total.QuadPart;
    }
    auto fmt = [](ULONGLONG b) {
        wchar_t s[32];
        double tb = b / (1024.0 * 1024.0 * 1024.0 * 1024.0);
        if (tb >= 0.9)
            swprintf_s(s, L"%.0fTB", tb + 0.5);
        else
            swprintf_s(s, L"%.0fGB", b / (1024.0 * 1024.0 * 1024.0) + 0.5);
        return std::wstring(s);
    };
    std::wstring r;
    auto add = [&](ULONGLONG b, const wchar_t* label) {
        if (!b)
            return;
        if (!r.empty())
            r += L" | ";
        r += fmt(b) + L" " + label;
    };
    add(nvmeBytes, L"NVMe");
    add(ssdBytes, L"SSD");
    add(hddBytes, L"HDD");
    return r.empty() ? L"Unknown Storage" : r;
}

// --- Thread 3: IO Worker ---
void IONVMeWorker() {
    (void)CoInitialize(NULL);
    IWICImagingFactory* factory = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    while (true) {
        SaveTask task;
        {
            std::unique_lock<std::mutex> lock(g_saveMutex);
            g_saveCV.wait(lock, [] { return !g_saveQueue.empty() || !g_running; });
            if (g_saveQueue.empty() && !g_running)
                break;
            task = std::move(g_saveQueue.front());
            g_saveQueue.pop();
        }
        bool writeOk = false;
        IWICBitmapEncoder* encoder = nullptr;
        IStream* stream = nullptr;
        if (SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) {
            if (SUCCEEDED(SHCreateStreamOnFileW(task.filePath.c_str(), STGM_CREATE | STGM_WRITE, &stream))) {
                encoder->Initialize(stream, WICBitmapEncoderNoCache);
                IWICBitmapFrameEncode* frame = nullptr;
                if (SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr))) {
                    frame->Initialize(nullptr);
                    frame->SetSize(task.width, task.height);
                    WICPixelFormatGUID format = GUID_WICPixelFormat24bppRGB;
                    frame->SetPixelFormat(&format);
                    HRESULT hrWrite = frame->WritePixels(task.height, task.rowPitch, (UINT)task.rgbData.size(), task.rgbData.data());
                    HRESULT hrCommitF = frame->Commit();
                    frame->Release();
                    HRESULT hrCommitE = encoder->Commit();
                    writeOk = SUCCEEDED(hrWrite) && SUCCEEDED(hrCommitF) && SUCCEEDED(hrCommitE);
                }
                stream->Release();
            }
            encoder->Release();
        }
        if (!writeOk) {
            g_ioFailures++;
            std::wcout << L"\n[!] IO FAILURE writing " << task.filePath << L" (total: " << g_ioFailures.load() << L")" << std::endl;
        }
    }
    if (factory)
        factory->Release();
    CoUninitialize();
}

// --- Thread 2: Processor ---
void ProcessingWorker(std::wofstream* logFile) {
    while (true) {
        RawFrame raw;
        {
            std::unique_lock<std::mutex> lock(g_rawMutex);
            g_rawCV.wait(lock, [] { return !g_rawQueue.empty() || !g_running; });
            if (g_rawQueue.empty() && !g_running)
                break;
            raw = std::move(g_rawQueue.front());
            g_rawQueue.pop();
        }
        SaveTask sTask;
        sTask.width = (UINT)raw.tw;
        sTask.height = (UINT)raw.th;
        sTask.rowPitch = sTask.width * 3;
        sTask.rgbData.resize((size_t)sTask.height * sTask.rowPitch);
        sTask.filePath = g_outputDir + raw.fileName;
        const BYTE* srcBase = reinterpret_cast<const BYTE*>(raw.bgraData);
        BYTE* dstPtr = sTask.rgbData.data();

        for (UINT y = 0; y < sTask.height; ++y) {
            const BYTE* sRow = srcBase + ((raw.ty + y) * raw.rowPitch) + (raw.tx * 4);
            for (UINT x = 0; x < sTask.width; ++x) {
                dstPtr[0] = sRow[0];
                dstPtr[1] = sRow[1];
                dstPtr[2] = sRow[2];
                sRow += 4;
                dstPtr += 3;
            }
        }
        {
            std::lock_guard<std::mutex> lock(g_saveMutex);
            g_saveQueue.push(std::move(sTask));
        }
        g_saveCV.notify_one();
        {
            std::lock_guard<std::mutex> lock(g_poolMutex);
            g_emptyPool.push(raw.poolIndex);
        }
    }
}

// --- Window Logic ---
LRESULT CALLBACK SelectorProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_LBUTTONDOWN:
            g_ptStartSel.x = (int)LOWORD(lParam);
            g_ptStartSel.y = (int)HIWORD(lParam);
            g_isDrawingSel = true;
            SetCapture(hWnd);
            return 0;
        case WM_MOUSEMOVE:
            if (g_isDrawingSel)
                InvalidateRect(hWnd, NULL, TRUE);
            return 0;
        case WM_LBUTTONUP:
            if (g_isDrawingSel) {
                POINT ptEnd;
                ptEnd.x = (int)LOWORD(lParam);
                ptEnd.y = (int)HIWORD(lParam);
                g_finalROI.x = min(g_ptStartSel.x, ptEnd.x);
                g_finalROI.y = min(g_ptStartSel.y, ptEnd.y);
                g_finalROI.w = abs(ptEnd.x - g_ptStartSel.x);
                g_finalROI.h = abs(ptEnd.y - g_ptStartSel.y);
                g_isDrawingSel = false;
                ReleaseCapture();
                PostQuitMessage(0);
            }
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc;
            GetClientRect(hWnd, &rc);
            HDC hMem = CreateCompatibleDC(hdc);
            HBITMAP hbm = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            SelectObject(hMem, hbm);
            HBRUSH hB = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(hMem, &rc, hB);
            DeleteObject(hB);
            if (g_isDrawingSel) {
                HPEN hP = CreatePen(PS_SOLID, 2, RGB(255, 0, 0));
                SelectObject(hMem, hP);
                SelectObject(hMem, GetStockObject(HOLLOW_BRUSH));
                POINT cur;
                GetCursorPos(&cur);
                ScreenToClient(hWnd, &cur);
                Rectangle(hMem, g_ptStartSel.x, g_ptStartSel.y, cur.x, cur.y);
                DeleteObject(hP);
            }
            BitBlt(hdc, 0, 0, rc.right, rc.bottom, hMem, 0, 0, SRCCOPY);
            DeleteObject(hbm);
            DeleteDC(hMem);
            EndPaint(hWnd, &ps);
            return 0;
        }
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE)
                PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

ROI SelectViewportWithMouse() {
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = SelectorProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"ROISelector";
    wc.hCursor = LoadCursor(NULL, IDC_CROSS);
    RegisterClassW(&wc);
    HWND hWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED,
        wc.lpszClassName,
        L"Select Viewport",
        WS_POPUP,
        0, 0,
        GetSystemMetrics(SM_CXSCREEN),
        GetSystemMetrics(SM_CYSCREEN),
        NULL, NULL, wc.hInstance, NULL);
    SetLayeredWindowAttributes(hWnd, 0, 150, LWA_ALPHA);
    ShowWindow(hWnd, SW_SHOW);
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0))
        DispatchMessage(&msg);
    DestroyWindow(hWnd);
    return g_finalROI;
}

// --- Main ---
int main(int argc, char* argv[]) {
    timeBeginPeriod(1);
    ROI myROI = { 0, 0, 0, 0 };
    std::wstring bDir = L"./";
    int dSec = 10;
    float iSec = 0.5f;
    int mIdx = 0;

    // --- 1. Mode Handling ---
    if (argc < 2) {
        myROI = SelectViewportWithMouse();
        if (myROI.w == 0 || myROI.h == 0)
            return 1;
    }
    else {
        std::string mode = argv[1];
        if (mode == "-c") {
            myROI = SelectViewportWithMouse();
            if (myROI.w > 0) {
                std::wofstream cfg(GetExeDir() + L"roi.cfg");
                cfg << myROI.x << L" " << myROI.y << L" " << myROI.w << L" " << myROI.h;
            }
            return 0;
        }
        if (mode == "-l") {
            std::wifstream cf(GetExeDir() + L"roi.cfg");
            if (!(cf >> myROI.x >> myROI.y >> myROI.w >> myROI.h))
                return 1;
        }
        else if (mode == "-m") {
            myROI = SelectViewportWithMouse();
            if (myROI.w == 0 || myROI.h == 0)
                return 1;
        }
        else if (mode == "-r" && argc >= 10) {
            myROI = { atoi(argv[6]), atoi(argv[7]), atoi(argv[8]), atoi(argv[9]) };
        }
        if (argc > 2) {
            wchar_t buf[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, argv[2], -1, buf, MAX_PATH);
            bDir = buf;
            if (bDir.back() != L'\\' && bDir.back() != L'/')
                bDir += L"\\";
        }
        if (argc > 3)
            dSec = atoi(argv[3]);
        if (argc > 4)
            iSec = (float)atof(argv[4]);
        if (argc > 5)
            mIdx = atoi(argv[5]);
    }

    (void)CoInitialize(NULL);
    time_t sNow = time(0);
    struct tm sTi;
    localtime_s(&sTi, &sNow);
    wchar_t sFN[128];
    swprintf_s(sFN, 128, L"Session_%04d%02d%02d_%02d%02d%02d\\",
        sTi.tm_year + 1900, sTi.tm_mon + 1, sTi.tm_mday,
        sTi.tm_hour, sTi.tm_min, sTi.tm_sec);

    wchar_t fB[MAX_PATH];
    if (_wfullpath(fB, bDir.c_str(), MAX_PATH) == nullptr)
        wcscpy_s(fB, MAX_PATH, bDir.c_str());
    g_outputDir = std::wstring(fB) + L"\\" + sFN;

    // --- 2. Create Directory & JSON ---
    SHCreateDirectoryExW(NULL, g_outputDir.c_str(), NULL);

    std::wstring cleanSID = sFN;
    if (!cleanSID.empty() && cleanSID.back() == L'\\')
        cleanSID.pop_back();

    std::wstring hardwareStr = GetCPUName() + L" | " + GetRAMSummary() + L" | " + GetStorageSummary() + L" | " + GetGPUNameDXGI();

    std::wofstream jLog(g_outputDir + L"session.json");
    if (jLog.is_open()) {
        jLog << L"{" << std::endl;
        jLog << L"  \"session_id\": \"" << cleanSID << L"\"," << std::endl;
        jLog << L"  \"system_res\": {\"w\":" << GetSystemMetrics(SM_CXSCREEN) << L", \"h\":" << GetSystemMetrics(SM_CYSCREEN) << L"}," << std::endl;
        jLog << L"  \"roi_pixels\": {\"x\":" << myROI.x << L", \"y\":" << myROI.y << L", \"w\":" << myROI.w << L", \"h\":" << myROI.h << L"}," << std::endl;
        jLog << L"  \"target_fps\": " << std::fixed << std::setprecision(1) << (1.0 / (double)iSec) << L"," << std::endl;
        jLog << L"  \"hardware\": \"" << hardwareStr << L"\"" << std::endl;
        jLog << L"}" << std::endl;
        jLog.close();
    }

    std::wofstream log(g_outputDir + L"metadata.csv");
    log << L"FrameIndex,FileName,TimestampMS,DeltaMS,NormX,NormY,NormW,NormH,ActiveWindow" << std::endl;

    // --- 3. Buffer Pool Init ---
    size_t stride = 4096 * 4;
    size_t frameBytes = stride * 2304;
    g_poolMemory = (BYTE*)VirtualAlloc(NULL, frameBytes * POOL_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    for (int i = 0; i < POOL_SIZE; i++)
        g_emptyPool.push(i);

    // --- 4. D3D11 Init ---
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };

    HRESULT hrDevice = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels, 2, D3D11_SDK_VERSION, &dev, nullptr, &ctx);

    if (FAILED(hrDevice) || !dev || !ctx)
        return 1;

    IDXGIDevice* dxDev = nullptr;
    dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxDev);
    IDXGIAdapter* adp = nullptr;
    dxDev->GetParent(__uuidof(IDXGIAdapter), (void**)&adp);
    IDXGIOutput* out = nullptr;
    adp->EnumOutputs(mIdx, &out);
    IDXGIOutput1* out1 = nullptr;
    out->QueryInterface(__uuidof(IDXGIOutput1), (void**)&out1);
    IDXGIOutputDuplication* dD = nullptr;
    out1->DuplicateOutput(dev, &dD);
    dxDev->Release();
    out1->Release();
    out->Release();
    adp->Release();

    // --- 5. Warm-up ---
    ID3D11Texture2D* persistentCopy = nullptr;
    for (int i = 0; i < 20; i++) {
        DXGI_OUTDUPL_FRAME_INFO fi;
        IDXGIResource* r = nullptr;
        if (SUCCEEDED(dD->AcquireNextFrame(50, &fi, &r)) && r) {
            r->Release();
            dD->ReleaseFrame();
        }
    }

    std::thread pT(ProcessingWorker, &log);
    std::thread iT(IONVMeWorker);

    double startT = GetQPCMS();
    double intervalMS = (double)iSec * 1000.0;
    double nextFrameT = 0;
    double lastTimestamp = 0;
    int fC = 1;
    ID3D11Texture2D* st = nullptr;

    // --- 6. Main Capture Loop ---
    while (((GetQPCMS() - startT) / 1000.0) < (double)dSec) {

        // --- Heartbeat & Buffer Health ---
        if (fC % 5 == 0) {
            double currentFPS = (fC - 1) / ((GetQPCMS() - startT) / 1000.0);
            size_t activeBuf = 0;
            {
                std::lock_guard<std::mutex> lk(g_poolMutex);
                activeBuf = POOL_SIZE - g_emptyPool.size();
            }
            std::wcout << L"\r[LIVE] Frame: " << std::setw(5) << (fC - 1)
                << L" | FPS: " << std::fixed << std::setprecision(1) << std::setw(4) << currentFPS
                << L" | Buffer: " << std::setw(3) << activeBuf << L"/" << POOL_SIZE
                << L" | Dropped: " << g_droppedFrames.load()
                << L" | IO Err: " << g_ioFailures.load() << L"   " << std::flush;
        }

        // --- Acquire (copy into persistent GPU texture before releasing frame) ---
        IDXGIResource* r = nullptr;
        DXGI_OUTDUPL_FRAME_INFO fi;
        if (SUCCEEDED(dD->AcquireNextFrame(0, &fi, &r)) && r) {
            ID3D11Texture2D* incoming = nullptr;
            if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&incoming)) && incoming) {
                if (!persistentCopy) {
                    D3D11_TEXTURE2D_DESC desc;
                    incoming->GetDesc(&desc);
                    desc.Usage = D3D11_USAGE_DEFAULT;
                    desc.CPUAccessFlags = 0;
                    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    desc.MiscFlags = 0;
                    dev->CreateTexture2D(&desc, nullptr, &persistentCopy);
                }
                if (persistentCopy)
                    ctx->CopyResource(persistentCopy, incoming);
                incoming->Release();
            }
            r->Release();
            dD->ReleaseFrame();
        }

        double nowMS = GetQPCMS() - startT;

        // --- Sampling Logic (Timer Triggered) ---
        if (nowMS >= nextFrameT && persistentCopy) {
            nextFrameT += intervalMS;

            if (!st) {
                D3D11_TEXTURE2D_DESC stagingDesc;
                persistentCopy->GetDesc(&stagingDesc);
                stagingDesc.Usage = D3D11_USAGE_STAGING;
                stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                stagingDesc.BindFlags = 0;
                stagingDesc.MiscFlags = 0;
                dev->CreateTexture2D(&stagingDesc, nullptr, &st);
            }

            if (st) {
                ctx->CopyResource(st, persistentCopy);
                ctx->Flush();

                D3D11_TEXTURE2D_DESC d;
                st->GetDesc(&d);
                D3D11_MAPPED_SUBRESOURCE m;
                if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
                    int slot = -1;
                    {
                        std::lock_guard<std::mutex> lock(g_poolMutex);
                        if (!g_emptyPool.empty()) {
                            slot = g_emptyPool.front();
                            g_emptyPool.pop();
                        }
                    }

                    if (slot != -1) {
                        RawFrame rf;
                        rf.poolIndex = slot;
                        rf.bgraData = g_poolMemory + (slot * frameBytes);
                        rf.width = d.Width;
                        rf.height = d.Height;
                        rf.rowPitch = m.RowPitch;
                        rf.tx = myROI.x;
                        rf.ty = myROI.y;
                        rf.tw = myROI.w;
                        rf.th = myROI.h;
                        rf.timestampMS = nowMS;
                        rf.frameIndex = fC;

                        memcpy(rf.bgraData, m.pData, (size_t)d.Height * m.RowPitch);

                        double sW = (double)GetSystemMetrics(SM_CXSCREEN);
                        double sH = (double)GetSystemMetrics(SM_CYSCREEN);
                        double delta = (fC == 1) ? 0 : (nowMS - lastTimestamp);
                        lastTimestamp = nowMS;

                        // --- Black Frame Watchdog (every 30 frames) ---
                        if (fC % 30 == 0 && rf.bgraData[0] == 0 && rf.bgraData[100] == 0 && rf.bgraData[1000] == 0)
                            std::wcout << L"\n[!] Frame " << fC << L": possible black frame detected" << std::endl;

                        // --- Timing Spike Detection ---
                        if (fC > 1 && delta > intervalMS * 2.0)
                            std::wcout << L"\n[!] Frame " << fC << L": timing spike " << std::fixed << std::setprecision(1) << delta << L"ms (expected ~" << intervalMS << L"ms)" << std::endl;

                        wchar_t fn[64];
                        swprintf_s(fn, 64, L"%04d_capture.png", fC);
                        rf.fileName = fn;

                        log << fC << L"," << rf.fileName << L"," << nowMS << L"," << delta << L","
                            << (double)myROI.x / sW << L"," << (double)myROI.y / sH << L","
                            << (double)myROI.w / sW << L"," << (double)myROI.h / sH << L",\"" << GetActiveWindowTitle() << L"\"" << std::endl;

                        {
                            std::lock_guard<std::mutex> lk(g_rawMutex);
                            g_rawQueue.push(rf);
                        }
                        g_rawCV.notify_one();
                        fC++;
                    }
                    else {
                        g_droppedFrames++;
                        std::wcout << L"\n[!] Frame " << fC << L": DROPPED (pool exhausted, " << g_droppedFrames.load() << L" total)" << std::endl;
                    }
                    ctx->Unmap(st, 0);
                }
            }
        }
        std::this_thread::yield();
    }

    // --- 7. Shutdown & Summary ---
    g_running = false;
    g_rawCV.notify_all();
    g_saveCV.notify_all();
    if (pT.joinable())
        pT.join();
    if (iT.joinable())
        iT.join();

    double sessionDuration = (GetQPCMS() - startT) / 1000.0;
    int totalCaptured = fC - 1;
    double actualFPS = (sessionDuration > 0.0) ? totalCaptured / sessionDuration : 0.0;

    std::wcout << L"\n\n=== SESSION COMPLETE ===" << std::endl;
    std::wcout << L"  Duration:        " << std::fixed << std::setprecision(2) << sessionDuration << L"s" << std::endl;
    std::wcout << L"  Frames captured: " << totalCaptured << std::endl;
    std::wcout << L"  Frames dropped:  " << g_droppedFrames.load() << std::endl;
    std::wcout << L"  IO failures:     " << g_ioFailures.load() << std::endl;
    std::wcout << L"  Actual FPS:      " << std::fixed << std::setprecision(2) << actualFPS << std::endl;

    log.close();

    std::wofstream jFinal(g_outputDir + L"session.json");
    if (jFinal.is_open()) {
        jFinal << L"{" << std::endl;
        jFinal << L"  \"session_id\": \"" << cleanSID << L"\"," << std::endl;
        jFinal << L"  \"system_res\": {\"w\":" << GetSystemMetrics(SM_CXSCREEN) << L", \"h\":" << GetSystemMetrics(SM_CYSCREEN) << L"}," << std::endl;
        jFinal << L"  \"roi_pixels\": {\"x\":" << myROI.x << L", \"y\":" << myROI.y << L", \"w\":" << myROI.w << L", \"h\":" << myROI.h << L"}," << std::endl;
        jFinal << L"  \"target_fps\": " << std::fixed << std::setprecision(1) << (1.0 / (double)iSec) << L"," << std::endl;
        jFinal << L"  \"hardware\": \"" << hardwareStr << L"\"," << std::endl;
        jFinal << L"  \"summary\": {" << std::endl;
        jFinal << L"    \"duration_s\": " << std::fixed << std::setprecision(3) << sessionDuration << L"," << std::endl;
        jFinal << L"    \"frames_captured\": " << totalCaptured << L"," << std::endl;
        jFinal << L"    \"frames_dropped\": " << g_droppedFrames.load() << L"," << std::endl;
        jFinal << L"    \"io_failures\": " << g_ioFailures.load() << L"," << std::endl;
        jFinal << L"    \"actual_fps\": " << std::fixed << std::setprecision(2) << actualFPS << std::endl;
        jFinal << L"  }" << std::endl;
        jFinal << L"}" << std::endl;
        jFinal.close();
    }

    VirtualFree(g_poolMemory, 0, MEM_RELEASE);
    timeEndPeriod(1);
    if (st)
        st->Release();
    if (persistentCopy)
        persistentCopy->Release();
    if (dD)
        dD->Release();
    if (ctx)
        ctx->Release();
    if (dev)
        dev->Release();
    CoUninitialize();
    if (argc < 2)
        system("pause");
    return 0;
}
