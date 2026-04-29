#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <iostream>
#include <fstream>
#include <string>
#include <ctime>
#include <vector>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

// --- Data Structures ---
struct RawFrame {
    std::vector<BYTE> bgraData;
    UINT width = 0, height = 0, rowPitch = 0;
    int tx = 0, ty = 0, tw = 0, th = 0;
    std::wstring fileName;
    ULONGLONG timestampMS = 0;
    int frameIndex = 0;
};

struct SaveTask {
    std::vector<BYTE> rgbData;
    UINT width = 0, height = 0, rowPitch = 0;
    std::wstring filePath;
};

struct ROI { int x = 0, y = 0, w = 0, h = 0; };

// --- Global State ---
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

// --- Thread 3: IO Worker ---
void IONVMeWorker() {
    (void)CoInitialize(NULL);
    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));

    if (SUCCEEDED(hr) && factory) {
        while (true) {
            SaveTask task;
            {
                std::unique_lock<std::mutex> lock(g_saveMutex);
                g_saveCV.wait(lock, [] { return !g_saveQueue.empty() || !g_running; });
                if (g_saveQueue.empty() && !g_running) break;
                task = std::move(g_saveQueue.front());
                g_saveQueue.pop();
            }

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
                        frame->WritePixels(task.height, task.rowPitch, (UINT)task.rgbData.size(), task.rgbData.data());
                        frame->Commit();
                        frame->Release();
                    }
                    encoder->Commit();
                    stream->Release();
                }
                encoder->Release();
            }
        }
        factory->Release();
    }
    CoUninitialize();
}

// --- Thread 2: Processor ---
void ProcessingWorker(std::wofstream* logFile) {
    while (true) {
        RawFrame raw;
        {
            std::unique_lock<std::mutex> lock(g_rawMutex);
            g_rawCV.wait(lock, [] { return !g_rawQueue.empty() || !g_running; });
            if (g_rawQueue.empty() && !g_running) break;
            raw = std::move(g_rawQueue.front());
            g_rawQueue.pop();
        }

        SaveTask sTask;
        sTask.width = (UINT)raw.tw;
        sTask.height = (UINT)raw.th;
        sTask.rowPitch = sTask.width * 3;
        sTask.rgbData.resize((size_t)sTask.height * sTask.rowPitch);
        sTask.filePath = g_outputDir + raw.fileName;

        const BYTE* srcBase = raw.bgraData.data();
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

        if (logFile && logFile->is_open()) {
            *logFile << raw.frameIndex << L"," << raw.fileName << L"," << raw.timestampMS << std::endl;
        }

        {
            std::lock_guard<std::mutex> lock(g_saveMutex);
            g_saveQueue.push(std::move(sTask));
        }
        g_saveCV.notify_one();
    }
}

// --- Selector Window Logic ---
LRESULT CALLBACK SelectorProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_LBUTTONDOWN:
        g_ptStartSel.x = (int)LOWORD(lParam); g_ptStartSel.y = (int)HIWORD(lParam);
        g_isDrawingSel = true; SetCapture(hWnd); return 0;
    case WM_MOUSEMOVE: if (g_isDrawingSel) InvalidateRect(hWnd, NULL, TRUE); return 0;
    case WM_LBUTTONUP:
        if (g_isDrawingSel) {
            POINT ptEnd = { (int)LOWORD(lParam), (int)HIWORD(lParam) };
            g_finalROI.x = min(g_ptStartSel.x, ptEnd.x); g_finalROI.y = min(g_ptStartSel.y, ptEnd.y);
            g_finalROI.w = abs(ptEnd.x - g_ptStartSel.x); g_finalROI.h = abs(ptEnd.y - g_ptStartSel.y);
            g_isDrawingSel = false; ReleaseCapture(); PostQuitMessage(0);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc; GetClientRect(hWnd, &rc);
        HDC hMem = CreateCompatibleDC(hdc); HBITMAP hbm = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        SelectObject(hMem, hbm);
        HBRUSH hB = CreateSolidBrush(RGB(0, 0, 0)); FillRect(hMem, &rc, hB); DeleteObject(hB);
        if (g_isDrawingSel) {
            HPEN hP = CreatePen(PS_SOLID, 2, RGB(255, 0, 0)); SelectObject(hMem, hP);
            SelectObject(hMem, GetStockObject(HOLLOW_BRUSH));
            POINT cur; GetCursorPos(&cur); ScreenToClient(hWnd, &cur);
            Rectangle(hMem, g_ptStartSel.x, g_ptStartSel.y, cur.x, cur.y);
            DeleteObject(hP);
        }
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, hMem, 0, 0, SRCCOPY);
        DeleteObject(hbm); DeleteDC(hMem); EndPaint(hWnd, &ps); return 0;
    }
    case WM_KEYDOWN: if (wParam == VK_ESCAPE) PostQuitMessage(0); return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

ROI SelectViewportWithMouse() {
    WNDCLASSW wc = { 0 }; wc.lpfnWndProc = SelectorProc; wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"ROISelector"; wc.hCursor = LoadCursor(NULL, IDC_CROSS);
    RegisterClassW(&wc);
    HWND hWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED, wc.lpszClassName, L"Select Viewport", WS_POPUP,
        0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), NULL, NULL, wc.hInstance, NULL);
    SetLayeredWindowAttributes(hWnd, 0, 150, LWA_ALPHA); ShowWindow(hWnd, SW_SHOW);
    MSG msg; while (GetMessage(&msg, NULL, 0, 0)) DispatchMessage(&msg);
    DestroyWindow(hWnd); return g_finalROI;
}

std::wstring GetExeDir() {
    wchar_t ep[MAX_PATH]; GetModuleFileNameW(NULL, ep, MAX_PATH);
    std::wstring s(ep); return s.substr(0, s.rfind(L'\\') + 1);
}

int RunConfigure() {
    ROI roi = SelectViewportWithMouse();
    if (roi.w == 0 || roi.h == 0) return 1;
    std::wofstream cfg(GetExeDir() + L"roi.cfg");
    cfg << roi.x << L" " << roi.y << L" " << roi.w << L" " << roi.h << std::endl;
    return 0;
}

// --- Main ---
int main(int argc, char* argv[]) {
    ROI myROI = { 0,0,0,0 };
    std::wstring bDir = L"./";
    int dSec = 10; float iSec = 0.5f; int mIdx = 0;

    if (argc < 2) {
        myROI = SelectViewportWithMouse();
        if (myROI.w == 0 || myROI.h == 0) return 1;
    }
    else {
        std::string mode = argv[1];
        if (mode == "-c") return RunConfigure();
        if (mode == "-l") {
            std::wifstream cf(GetExeDir() + L"roi.cfg");
            if (!(cf >> myROI.x >> myROI.y >> myROI.w >> myROI.h)) return 1;
        }
        else if (mode == "-m") {
            myROI = SelectViewportWithMouse();
            if (myROI.w == 0 || myROI.h == 0) return 1;
        }
        else if (mode == "-r" && argc >= 10) {
            myROI = { atoi(argv[6]), atoi(argv[7]), atoi(argv[8]), atoi(argv[9]) };
        }
        if (argc > 2) {
            wchar_t buf[MAX_PATH]; MultiByteToWideChar(CP_ACP, 0, argv[2], -1, buf, MAX_PATH);
            bDir = buf; if (bDir.back() != L'\\' && bDir.back() != L'/') bDir += L"\\";
        }
        if (argc > 3) dSec = atoi(argv[3]);
        if (argc > 4) iSec = (float)atof(argv[4]);
        if (argc > 5) mIdx = atoi(argv[5]);
    }

    (void)CoInitialize(NULL);

    time_t sNow = time(0); struct tm sTi; localtime_s(&sTi, &sNow);
    wchar_t sFN[128]; swprintf_s(sFN, 128, L"Session_%04d%02d%02d_%02d%02d%02d\\", sTi.tm_year + 1900, sTi.tm_mon + 1, sTi.tm_mday, sTi.tm_hour, sTi.tm_min, sTi.tm_sec);
    wchar_t fB[MAX_PATH]; if (_wfullpath(fB, bDir.c_str(), MAX_PATH) == nullptr) wcscpy_s(fB, MAX_PATH, bDir.c_str());
    g_outputDir = std::wstring(fB) + L"\\" + sFN;
    SHCreateDirectoryExW(NULL, g_outputDir.c_str(), NULL);

    std::wofstream log(g_outputDir + L"metadata.csv");
    log << L"FrameIndex,FileName,TimestampMS" << std::endl;

    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    HRESULT hrDevice = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hrDevice) || dev == nullptr || ctx == nullptr) return 1;

    IDXGIDevice* dxDev = nullptr; dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxDev);
    IDXGIAdapter* adp = nullptr; dxDev->GetParent(__uuidof(IDXGIAdapter), (void**)&adp);
    IDXGIOutput* out = nullptr; adp->EnumOutputs(mIdx, &out);
    IDXGIOutput1* out1 = nullptr; out->QueryInterface(__uuidof(IDXGIOutput1), (void**)&out1);
    IDXGIOutputDuplication* dD = nullptr; out1->DuplicateOutput(dev, &dD);
    if (dxDev) dxDev->Release(); if (out1) out1->Release(); if (out) out->Release(); if (adp) adp->Release();

    // --- Warm-up flush ---
    DXGI_OUTDUPL_FRAME_INFO warmInfo; IDXGIResource* warmRes = nullptr;
    for (int i = 0; i < 10; i++) {
        if (SUCCEEDED(dD->AcquireNextFrame(100, &warmInfo, &warmRes))) {
            dD->ReleaseFrame(); if (warmRes) warmRes->Release();
        }
    }

    std::thread pT(ProcessingWorker, &log);
    std::thread iT(IONVMeWorker);

    ULONGLONG sT64 = GetTickCount64();
    ULONGLONG intervalMS = (ULONGLONG)(iSec * 1000.0f);
    ULONGLONG lS = 0; int fC = 1;

    while (((GetTickCount64() - sT64) / 1000) < (ULONGLONG)dSec) {
        IDXGIResource* r = nullptr; DXGI_OUTDUPL_FRAME_INFO fi;
        if (SUCCEEDED(dD->AcquireNextFrame(10, &fi, &r)) && r) {
            ULONGLONG nMS = GetTickCount64() - sT64;
            if (fC == 1 || (nMS - lS >= intervalMS)) {
                ID3D11Texture2D* gT = nullptr;
                if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&gT)) && gT) {
                    D3D11_TEXTURE2D_DESC d; gT->GetDesc(&d);
                    d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    d.BindFlags = 0; d.MiscFlags = 0;
                    ID3D11Texture2D* st = nullptr;
                    if (SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &st)) && st) {
                        ctx->CopyResource(st, gT);
                        D3D11_MAPPED_SUBRESOURCE m;
                        if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
                            RawFrame rf; rf.width = d.Width; rf.height = d.Height; rf.rowPitch = m.RowPitch;
                            rf.tx = myROI.x; rf.ty = myROI.y; rf.tw = myROI.w; rf.th = myROI.h;
                            rf.timestampMS = nMS; rf.frameIndex = fC++;
                            wchar_t fn[128]; swprintf_s(fn, 128, L"%04d_capture.png", rf.frameIndex);
                            rf.fileName = fn;
                            rf.bgraData.assign((BYTE*)m.pData, (BYTE*)m.pData + (d.Height * m.RowPitch));
                            { std::lock_guard<std::mutex> lk(g_rawMutex); g_rawQueue.push(std::move(rf)); }
                            g_rawCV.notify_one();
                            ctx->Unmap(st, 0); lS = nMS;
                        }
                        if (st) st->Release();
                    }
                    if (gT) gT->Release();
                }
            }
            r->Release(); dD->ReleaseFrame();
        }
        Sleep(1);
    }

    { std::lock_guard<std::mutex> lk(g_rawMutex); g_running = false; }
    g_rawCV.notify_all(); g_saveCV.notify_all();
    if (pT.joinable()) pT.join(); if (iT.joinable()) iT.join();

    // --- Final Output Block ---
    wchar_t fullPath[MAX_PATH];
    if (_wfullpath(fullPath, g_outputDir.c_str(), MAX_PATH)) {
        std::wcout << L"\n========================================" << std::endl;
        std::wcout << L"Capture Session Complete!" << std::endl;
        std::wcout << L"Total Frames: " << fC - 1 << std::endl;
        std::wcout << L"Output Folder: " << fullPath << std::endl;
        std::wcout << L"========================================\n" << std::endl;
    }

    log.close();
    if (dD) dD->Release(); if (ctx) ctx->Release(); if (dev) dev->Release();
    CoUninitialize();
    if (argc < 2) system("pause");
    return 0;
}
