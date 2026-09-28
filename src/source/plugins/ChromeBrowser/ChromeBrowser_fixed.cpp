#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "ChromeBrowser.h"
#include "ChromeBrowserThumbnail.h"

#include <combaseapi.h>

#include <WebView2.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <wincodec.h>

#include <ffglex/FFGLScopedShaderBinding.h>
#include <ffglex/FFGLScopedSamplerActivation.h>
#include <ffglex/FFGLScopedTextureBinding.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>

using namespace ffglex;
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

// Toolbar button IDs
#define IDC_BTN_BACK    100
#define IDC_BTN_FORWARD 101
#define IDC_BTN_HOME    102

// ---------------------------------------------------------------------------
// DllMain — track load/unload (no C++ statics in DETACH)
// ---------------------------------------------------------------------------
#pragma warning(push)
#pragma warning(disable : 4996) // wcscpy_s / wcsrchr
static wchar_t sDllPath[MAX_PATH] = {};
extern "C" BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        GetModuleFileNameW(hInst, sDllPath, MAX_PATH);
        wchar_t logPath[MAX_PATH] = {};
        wcscpy(logPath, sDllPath);
        wchar_t* sl = wcsrchr(logPath, L'\\');
        if (sl) wcscpy(sl + 1, L"chromebrowser_dll.txt");

        FILE* f = nullptr;
        _wfopen_s(&f, logPath, L"a");
        if (f) { fwprintf(f, L"DLL LOADED: %s\n", sDllPath); fclose(f); }
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        wchar_t logPath[MAX_PATH] = {};
        wcscpy(logPath, sDllPath);
        wchar_t* sl = wcsrchr(logPath, L'\\');
        if (sl) wcscpy(sl + 1, L"chromebrowser_dll.txt");

        FILE* f = nullptr;
        _wfopen_s(&f, logPath, L"a");
        if (f) { fwprintf(f, L"DLL UNLOADED\n"); fclose(f); }
    }
    return TRUE;
}
#pragma warning(pop)

// ---------------------------------------------------------------------------
// Plugin registration
// ---------------------------------------------------------------------------
static CFFGLPluginInfo PluginInfo(
    PluginFactory< ChromeBrowser >,
    "CHRM", "Chrome Browser", 2, 1, 0, 0,
    FF_SOURCE,
    "Embeds a WebView2 browser. URL (text) + Show Window (toggle).",
    "github.com/odkkirova/ffglweb"
);

// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------
static const char kVert[] = R"(#version 410 core
layout(location=0) in vec2 vPos;
layout(location=1) in vec2 vUV;
out vec2 uv;
void main() { gl_Position = vec4(vPos,0,1); uv = vUV; }
)";

static const char kFrag[] = R"(#version 410 core
uniform sampler2D tex;
in vec2 uv;
out vec4 fragColor;
void main() { fragColor = texture(tex, uv); }
)";

// ---------------------------------------------------------------------------
// Logging (no-op, logs removed)
// ---------------------------------------------------------------------------
static void MLog(const std::string&) {}

// ---------------------------------------------------------------------------
// ChromeBrowser
// ---------------------------------------------------------------------------

ChromeBrowser::ChromeBrowser() : CFFGLPlugin()
{
    MLog("=== ChromeBrowser constructor ===");
    mLastCaptureTime = std::chrono::steady_clock::now();
    SetMinInputs(0);
    SetMaxInputs(0);

    SetParamInfo(PARAM_URL,         "URL",         FF_TYPE_TEXT,    "https://www.google.com");
    SetParamInfo(PARAM_SHOW_WINDOW, "Toggle Window", FF_TYPE_EVENT, 0.0f);
    SetParamInfo(PARAM_GO,          "Go",          FF_TYPE_EVENT,   0.0f);
}

ChromeBrowser::~ChromeBrowser()
{
    MLog("=== ChromeBrowser destructor ===");
    DestroyWebView();
}

// ---------------------------------------------------------------------------
// Helper: restore foreground to a target window (cross-process safe)
// ---------------------------------------------------------------------------
static void RestoreForeground(HWND target)
{
    if (!target) return;
    DWORD targetTid = GetWindowThreadProcessId(target, nullptr);
    DWORD ourTid    = GetCurrentThreadId();
    if (targetTid == ourTid || targetTid == 0) return;

    AttachThreadInput(ourTid, targetTid, TRUE);
    SetForegroundWindow(target);
    BringWindowToTop(target);
    SetFocus(target);
    AttachThreadInput(ourTid, targetTid, FALSE);
}

// ---------------------------------------------------------------------------
// Window proc
// ---------------------------------------------------------------------------
LRESULT CALLBACK ChromeBrowser::WndProc( HWND hWnd, UINT msg,
                                         WPARAM wParam, LPARAM lParam )
{
    ChromeBrowser* self = reinterpret_cast<ChromeBrowser*>(
        GetWindowLongPtrW(hWnd, GWLP_USERDATA) );

    if (msg == WM_CLOSE && self)
    {
        self->mWindowVisible = false;
        ShowWindow(hWnd, SW_HIDE);
        return 0;
    }
    if (msg == WM_DESTROY && self)
    {
        self->mWindowVisible = false;
        return 0;
    }
    if (msg == WM_COMMAND && self)
    {
        switch (LOWORD(wParam))
        {
        case IDC_BTN_BACK:
            if (self->mWebView) self->mWebView->GoBack();
            return 0;
        case IDC_BTN_FORWARD:
            if (self->mWebView) self->mWebView->GoForward();
            return 0;
        case IDC_BTN_HOME:
            if (self->mWebView) { self->mNeedsNav = true; self->mURL = "https://www.google.com"; }
            return 0;
        }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// WebView2 lifecycle
// ---------------------------------------------------------------------------
void ChromeBrowser::CreateWebView()
{
    if (mWebViewInit) return;

    const wchar_t kWndClass[] = L"ChromeBrowserFFGL_WC";
    if (!mWndClassRegd)
    {
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = WndProc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = kWndClass;
        if (!RegisterClassExW(&wc))
            MLog("RegisterClassExW failed: " + std::to_string(GetLastError()));
        mWndClassRegd = true;
    }

    RECT wr = {0, 0, 1200, 800};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    mHiddenWnd = CreateWindowExW(
        0, kWndClass, L"Chrome Browser (FFGL)",
        WS_OVERLAPPEDWINDOW,
        -10000, -10000,
        wr.right - wr.left, wr.bottom - wr.top,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr );

    if (!mHiddenWnd)
    {
        MLog("CreateWindowExW failed: " + std::to_string(GetLastError()));
        return;
    }

    SetWindowLongPtrW(mHiddenWnd, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(this));

    // Create toolbar buttons
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    mBtnBack    = CreateWindowW(L"BUTTON", L"\u25C0",
                    WS_CHILD | BS_PUSHBUTTON,
                    2, 2, 30, TOOLBAR_H - 4,
                    mHiddenWnd, (HMENU)IDC_BTN_BACK, hInst, nullptr);
    mBtnForward = CreateWindowW(L"BUTTON", L"\u25B6",
                    WS_CHILD | BS_PUSHBUTTON,
                    34, 2, 30, TOOLBAR_H - 4,
                    mHiddenWnd, (HMENU)IDC_BTN_FORWARD, hInst, nullptr);
    mBtnHome    = CreateWindowW(L"BUTTON", L"\u2302",
                    WS_CHILD | BS_PUSHBUTTON,
                    66, 2, 30, TOOLBAR_H - 4,
                    mHiddenWnd, (HMENU)IDC_BTN_HOME, hInst, nullptr);

    // Show toolbar buttons even when window is offscreen
    ShowWindow(mBtnBack, SW_SHOW);
    ShowWindow(mBtnForward, SW_SHOW);
    ShowWindow(mBtnHome, SW_SHOW);

    // COM init — WebView2 requires STA
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr))
        mComInitialized = true;
    else if (hr != RPC_E_CHANGED_MODE)
        MLog("CoInitializeEx failed: " + std::to_string(hr));

    // User data folder next to plugin DLL
    wchar_t dataPath[MAX_PATH] = {};
    {
        static int sAnchor = 0;
        HMODULE hm = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)&sAnchor, &hm))
        {
            GetModuleFileNameW(hm, dataPath, MAX_PATH);
            wchar_t* sl = wcsrchr(dataPath, L'\\');
            if (sl) wcscpy_s(sl+1, MAX_PATH-(sl-dataPath)-1,
                             L"chromebrowser_data");
        }
    }

    HRESULT envHr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        dataPath[0] ? dataPath : nullptr,
        nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT res, ICoreWebView2Environment* env) -> HRESULT
            {
                if (FAILED(res))
                {
                    MLog("WebView2 env FAILED: " + std::to_string(res));
                    return res;
                }
                ComPtr<ICoreWebView2Environment> spEnv(env);

                return spEnv->CreateCoreWebView2Controller(
                    mHiddenWnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this](HRESULT res2,
                               ICoreWebView2Controller* ctl) -> HRESULT
                        {
                            if (FAILED(res2))
                            {
                                MLog("WebView2 ctl FAILED: "
                                     + std::to_string(res2));
                                return res2;
                            }

                            mController = ctl;
                            mController->get_CoreWebView2(&mWebView);

                            mController->put_IsVisible(TRUE);
                            RECT r = {0, TOOLBAR_H, 1920, 1080 + TOOLBAR_H};
                            mController->put_Bounds(r);

                            // Navigation completed → capture will start
                            // on next ProcessOpenGL call
                            mWebView->add_NavigationCompleted(
                                Callback<
                                  ICoreWebView2NavigationCompletedEventHandler>(
                                    [this](ICoreWebView2*,
                                           ICoreWebView2NavigationCompletedEventArgs*)
                                        -> HRESULT
                                    {
                                        mNeedsFreshCapture = true;
                                        mFreshCaptureAfter =
                                            std::chrono::steady_clock::now()
                                            + std::chrono::milliseconds(100);
                                        return S_OK;
                                    }).Get(),
                                nullptr);

                            // Intercept new window requests (target=_blank,
                            // window.open, etc.) — redirect to main WebView
                            mWebView->add_NewWindowRequested(
                                Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                    [this](ICoreWebView2*,
                                           ICoreWebView2NewWindowRequestedEventArgs* args)
                                        -> HRESULT
                                    {
                                        LPWSTR uri = nullptr;
                                        if (SUCCEEDED(args->get_Uri(&uri)) && uri)
                                        {
                                            mWebView->Navigate(uri);
                                            CoTaskMemFree(uri);
                                        }
                                        args->put_Handled(TRUE);
                                        return S_OK;
                                    }).Get(),
                                nullptr);

                            mWebViewInit = true;
                            MLog("WebView2 initialized OK");

                            Navigate();
                            return S_OK;
                        }).Get());
            }).Get());

    if (FAILED(envHr))
        MLog("CreateEnvironment FAILED: " + std::to_string(envHr));
}

void ChromeBrowser::DestroyWebView()
{
    mWebViewInit = false;
    mNeedsNav    = false;

    // Drain pending WebView2 callbacks before Stop/Close
    {
        MSG msg;
        auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(200);
        while (std::chrono::steady_clock::now() < deadline)
        {
            bool had = false;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                had = true;
            }
            if (!had) break;
        }
    }

    if (mWebView)
    {
        mWebView->Stop();
        mWebView.Reset();
    }
    if (mController)
    {
        mController->Close();
        mController.Reset();
    }

    if (mHiddenWnd)
    {
        DestroyWindow(mHiddenWnd);
        mHiddenWnd = nullptr;
    }

    MLog("WebView2 destroyed");
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------
void ChromeBrowser::Navigate()
{
    if (!mWebViewInit || !mWebView)
    {
        mNeedsNav = true;
        return;
    }

    std::string url = mURL;
    if (url.find("://") == std::string::npos)
        url = "https://" + url;

    MLog("Navigate: " + url);
    std::wstring wurl(url.begin(), url.end());
    mWebView->Navigate(wurl.c_str());
    mNeedsNav = false;
}

// ---------------------------------------------------------------------------
// Show / hide
// ---------------------------------------------------------------------------
void ChromeBrowser::ToggleWindow()
{
    if (!mHiddenWnd) return;

    mWindowVisible = !mWindowVisible;
    if (mWindowVisible)
    {
        int cx = GetSystemMetrics(SM_CXSCREEN);
        int cy = GetSystemMetrics(SM_CYSCREEN);

        mArenaHwnd = GetForegroundWindow();

        RECT wr = {0, 0, 1200, 800};
        AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
        int w = wr.right - wr.left;
        int h = wr.bottom - wr.top;

        SetWindowPos(mHiddenWnd, HWND_TOP,
                     (cx - w) / 2, (cy - h) / 2,
                     w, h, SWP_SHOWWINDOW);
        ShowWindow(mHiddenWnd, SW_SHOW);
        SetForegroundWindow(mHiddenWnd);
        MLog("Window shown");
    }
    else
    {
        ShowWindow(mHiddenWnd, SW_HIDE);
        AllowSetForegroundWindow(ASFW_ANY);
        if (mArenaHwnd) SetForegroundWindow(mArenaHwnd);
        mWindowVisible = false;
        MLog("Window hidden");
    }
}

// ---------------------------------------------------------------------------
// Capture frame (called from WM_TIMER)
// ---------------------------------------------------------------------------
void ChromeBrowser::TriggerCapture()
{
    if (!mWebViewInit || !mWebView) return;
    if (mCapturePending) return;

    HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, 0);
    if (!hGlobal) return;

    ComPtr<IStream> stream;
    HRESULT hr = CreateStreamOnHGlobal(hGlobal, TRUE, &stream);
    if (FAILED(hr))
    {
        GlobalFree(hGlobal);
        return;
    }

    // Cache WIC factory once
    if (!mWICFactory)
    {
        CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                         CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mWICFactory));
    }

    mCapturePending = true;
    mCapturePendingSince = std::chrono::steady_clock::now();
    hr = mWebView->CapturePreview(
        COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_JPEG,
        stream.Get(),
        Callback<ICoreWebView2CapturePreviewCompletedHandler>(
            [this, stream](HRESULT res) -> HRESULT
            {
                mCapturePending = false;
                if (FAILED(res)) return res;

                // Read entire stream into mJpegBuffer
                STATSTG stat = {};
                if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)))
                    return S_OK;

                ULARGE_INTEGER sz = stat.cbSize;
                if (sz.HighPart || sz.LowPart == 0)
                    return S_OK;

                LARGE_INTEGER zero{};
                stream->Seek(zero, STREAM_SEEK_SET, nullptr);

                mJpegBuffer.resize(sz.LowPart);
                ULONG read = 0;
                if (FAILED(stream->Read(mJpegBuffer.data(), sz.LowPart, &read)))
                    return S_OK;

                mJpegReady = true;
                return S_OK;
            }).Get());

    if (FAILED(hr))
    {
        mCapturePending = false;
    }
}

// ---------------------------------------------------------------------------
// Decode JPEG → BGRA (called from ProcessOpenGL after callback copies data)
// ---------------------------------------------------------------------------
void ChromeBrowser::DecodeJpeg()
{
    if (mJpegBuffer.empty() || !mWICFactory) return;

    ComPtr<IStream> stream;
    HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, mJpegBuffer.size());
    if (!hGlobal) return;

    void* ptr = GlobalLock(hGlobal);
    if (ptr)
    {
        memcpy(ptr, mJpegBuffer.data(), mJpegBuffer.size());
        GlobalUnlock(hGlobal);
    }

    if (FAILED(CreateStreamOnHGlobal(hGlobal, TRUE, &stream)))
    {
        GlobalFree(hGlobal);
        return;
    }

    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(mWICFactory->CreateDecoderFromStream(stream.Get(), nullptr,
            WICDecodeMetadataCacheOnLoad, &dec)))
        return;

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame)))
        return;

    UINT w = 0, h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0)
        return;

    ComPtr<IWICFormatConverter> conv;
    if (FAILED(mWICFactory->CreateFormatConverter(&conv)))
        return;

    if (FAILED(conv->Initialize(frame.Get(),
            GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeCustom)))
        return;

    UINT stride = w * 4;
    UINT bufSize = stride * h;
    mDecodeBuffer.resize(bufSize);

    if (FAILED(conv->CopyPixels(nullptr, stride, bufSize, mDecodeBuffer.data())))
        return;

    {
        std::lock_guard<std::mutex> lk(mCaptureMutex);
        mCaptured.w = w;
        mCaptured.h = h;
        mCaptured.pixels.swap(mDecodeBuffer);
        mCaptured.fresh = true;
    }

    mNeedsTriggerCapture = true;
}

// ---------------------------------------------------------------------------
// FFGL
// ---------------------------------------------------------------------------
FFResult ChromeBrowser::InitGL(const FFGLViewportStruct* vp)
{
    MLog("=== InitGL ===");

    if (!mShader.Compile(kVert, kFrag))
    {
        MLog("Shader FAILED");
        DeInitGL();
        return FF_FAIL;
    }

    float verts[] = {
        -1,-1, 0,1,
         1,-1, 1,1,
        -1, 1, 0,0,
         1, 1, 1,0
    };
    glGenVertexArrays(1, &mVAO);
    glGenBuffers(1, &mVBO);
    glBindVertexArray(mVAO);
    glBindBuffer(GL_ARRAY_BUFFER, mVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float)));
    glBindVertexArray(0);

    // Fallback 1x1 black texture
    glGenTextures(1, &mTexture);
    glBindTexture(GL_TEXTURE_2D, mTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    uint32_t fallback = 0xFF000000; // black (BGRA)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0,
                 GL_BGRA, GL_UNSIGNED_BYTE, &fallback);
    glBindTexture(GL_TEXTURE_2D, 0);

    mShaderReady = true;
    mArenaHwnd = GetForegroundWindow();
    CreateWebView();
    MLog("InitGL complete");
    return FF_SUCCESS;
}

FFResult ChromeBrowser::DeInitGL()
{
    MLog("=== DeInitGL ===");
    DestroyWebView();
    mShader.FreeGLResources();
    if (mVAO)     { glDeleteVertexArrays(1, &mVAO); mVAO = 0; }
    if (mVBO)     { glDeleteBuffers(1, &mVBO); mVBO = 0; }
    if (mTexture) { glDeleteTextures(1, &mTexture); mTexture = 0; }
    mTexW = mTexH = 0;
    mShaderReady = false;
    return FF_SUCCESS;
}

FFResult ChromeBrowser::ProcessOpenGL(ProcessOpenGLStruct* pGL)
{
    if (!mShaderReady) return FF_FAIL;

    // Pump messages so WebView2 async callbacks fire
    MSG msg = {};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (mNeedsNav && mWebViewInit)
        Navigate();

    // Pipeline: decode JPEG captured by callback, then chain next capture
    if (mJpegReady)
    {
        mJpegReady = false;
        DecodeJpeg();
    }

    if (mNeedsTriggerCapture && mWebViewInit && !mCapturePending)
    {
        mNeedsTriggerCapture = false;
        TriggerCapture();
    }

    // Safety: force-reset stuck capture after 3 s
    if (mCapturePending && mWebViewInit)
    {
        auto now = std::chrono::steady_clock::now();
        auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
                         now - mCapturePendingSince).count();
        if (since >= 3000)
        {
            mCapturePending = false;
            MLog("Capture stuck — force-reset after "
                 + std::to_string(since) + "ms");
        }
    }

    // Fresh capture after navigation (with small delay for page to start rendering)
    if (mNeedsFreshCapture && mWebViewInit && !mCapturePending)
    {
        auto now = std::chrono::steady_clock::now();
        if (now >= mFreshCaptureAfter)
        {
            mNeedsFreshCapture = false;
            TriggerCapture();
        }
    }

    // One-shot initial / rate-limited fallback
    if (mWebViewInit && !mCapturePending && !mNeedsTriggerCapture && !mJpegReady)
    {
        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now - mLastCaptureTime).count();
        if (ms >= 16)
        {
            mLastCaptureTime = now;
            TriggerCapture();
        }
    }

    // Swap captured frame
    {
        std::lock_guard<std::mutex> lk(mCaptureMutex);
        if (mCaptured.fresh)
        {
            mUploadW = mCaptured.w;
            mUploadH = mCaptured.h;
            mUploadPixels.swap(mCaptured.pixels);
            mCaptured.fresh = false;
        }
    }

    // Upload to GPU
    if (mUploadW && mUploadH && !mUploadPixels.empty())
    {
        glBindTexture(GL_TEXTURE_2D, mTexture);
        if (mUploadW != mTexW || mUploadH != mTexH)
        {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                         mUploadW, mUploadH, 0,
                         GL_BGRA, GL_UNSIGNED_BYTE,
                         mUploadPixels.data());
            mTexW = mUploadW;
            mTexH = mUploadH;
        }
        else
        {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                            mUploadW, mUploadH,
                            GL_BGRA, GL_UNSIGNED_BYTE,
                            mUploadPixels.data());
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        mUploadW = mUploadH = 0;
    }

    // Render
    if (mTexture)
    {
        ScopedShaderBinding sb(mShader.GetGLID());
        ScopedSamplerActivation sa(0);
        ScopedTextureBinding tb(GL_TEXTURE_2D, mTexture);
        mShader.Set("tex", 0);
        glBindVertexArray(mVAO);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
    }

    return FF_SUCCESS;
}

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------
FFResult ChromeBrowser::SetFloatParameter(unsigned int index, float value)
{
    if (index == PARAM_SHOW_WINDOW)
    {
        if (value != 0.0f)
            ToggleWindow();
        return FF_SUCCESS;
    }
    if (index == PARAM_GO)
    {
        if (value != 0.0f)
            mNeedsNav = true;
        return FF_SUCCESS;
    }
    return FF_FAIL;
}

float ChromeBrowser::GetFloatParameter(unsigned int index)
{
    if (index == PARAM_SHOW_WINDOW)
        return 0.0f; // event type — always 0
    return 0.0f;
}

FFResult ChromeBrowser::SetTextParameter(unsigned int index, const char* value)
{
    if (index == PARAM_URL && value && value[0] != '\0')
    {
        mURL = value;
        MLog("URL set: " + mURL);
        return FF_SUCCESS;
    }
    return FF_FAIL;
}

char* ChromeBrowser::GetTextParameter(unsigned int index)
{
    if (index == PARAM_URL)
        return const_cast<char*>(mURL.c_str());
    return nullptr;
}
