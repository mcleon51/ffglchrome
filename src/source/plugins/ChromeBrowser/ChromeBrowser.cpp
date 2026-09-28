#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "ChromeBrowser.h"

// WIN32_LEAN_AND_MEAN excludes RPC/COM headers that WebView2.h needs.
#include <rpc.h>
#include <rpcndr.h>
#include <objbase.h>

#ifndef interface
#define interface struct
#endif

#include <WebView2.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <ffglex/FFGLScopedShaderBinding.h>
#include <ffglex/FFGLScopedSamplerActivation.h>
#include <ffglex/FFGLScopedTextureBinding.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <string>

using namespace ffglex;
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

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
// Logging
// ---------------------------------------------------------------------------
static std::mutex  sLogMtx;
static std::string sLogPath;

static void MLog(const std::string& msg)
{
    std::lock_guard<std::mutex> lk(sLogMtx);
    if (sLogPath.empty())
    {
        wchar_t tmp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tmp);
        sLogPath = std::string(tmp, tmp + wcslen(tmp)) + "chromebrowser_debug.txt";
    }
    std::ofstream f(sLogPath, std::ios::app);
    if (f) { f << msg << "\n"; f.flush(); }
}

// ---------------------------------------------------------------------------
// ChromeBrowser
// ---------------------------------------------------------------------------

ChromeBrowser::ChromeBrowser() : CFFGLPlugin()
{
    MLog("=== ChromeBrowser constructor ===");
    SetMinInputs(0);
    SetMaxInputs(0);

    SetParamInfof(PARAM_URL,         "URL",         FF_TYPE_TEXT );
    SetParamInfof(PARAM_SHOW_WINDOW, "Show Window", FF_TYPE_BOOLEAN );
}

ChromeBrowser::~ChromeBrowser()
{
    MLog("=== ChromeBrowser destructor ===");
    DestroyWebView();
}

// ---------------------------------------------------------------------------
// Window proc
// ---------------------------------------------------------------------------
LRESULT CALLBACK ChromeBrowser::WndProc( HWND hWnd, UINT msg,
                                         WPARAM wParam, LPARAM lParam )
{
    ChromeBrowser* self = reinterpret_cast<ChromeBrowser*>(
        GetWindowLongPtrW(hWnd, GWLP_USERDATA) );

    if (msg == WM_TIMER && wParam == kCaptureTimerId && self)
    {
        self->TriggerCapture();
        return 0;
    }
    if (msg == WM_CLOSE && self)
    {
        self->mWindowVisible = false;
        self->mShowWindow    = 0.0f;
        ShowWindow(hWnd, SW_HIDE);
        return 0;
    }
    if (msg == WM_DESTROY && self)
    {
        self->mWindowVisible = false;
        self->mShowWindow    = 0.0f;
        return 0;
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

    mHiddenWnd = CreateWindowExW(
        0, kWndClass, L"Chrome Browser (FFGL)",
        WS_POPUP,
        -10000, -10000, 1280, 720,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr );

    if (!mHiddenWnd)
    {
        MLog("CreateWindowExW failed: " + std::to_string(GetLastError()));
        return;
    }

    SetWindowLongPtrW(mHiddenWnd, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(this));

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

    // Create environment options
    ComPtr<ICoreWebView2EnvironmentOptions> envOpts;
    CreateCoreWebView2EnvironmentOptions(nullptr, nullptr, nullptr, &envOpts);

    HRESULT envHr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        dataPath[0] ? dataPath : nullptr,
        envOpts.Get(),
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
                            RECT r = {0, 0, 1280, 720};
                            mController->put_Bounds(r);

                            // Navigation completed → start capture timer
                            mWebView->add_NavigationCompleted(
                                Callback<
                                  ICoreWebView2NavigationCompletedEventHandler>(
                                    [this](ICoreWebView2*,
                                           ICoreWebView2NavigationCompletedEventArgs*)
                                        -> HRESULT
                                    {
                                        MLog("Navigation completed");
                                        SetTimer(mHiddenWnd,
                                                 kCaptureTimerId,
                                                 33, nullptr);
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
    if (mHiddenWnd)
        KillTimer(mHiddenWnd, kCaptureTimerId);

    mWebViewInit = false;
    mNeedsNav    = false;

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

    if (mComInitialized) { CoUninitialize(); mComInitialized = false; }
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

        RECT bounds = {};
        mController->get_Bounds(&bounds);
        int w = bounds.right  - bounds.left;
        int h = bounds.bottom - bounds.top;
        if (w <= 0) { w = 1280; h = 720; }

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
        MLog("Window hidden");
    }
}

// ---------------------------------------------------------------------------
// Capture frame (called from WM_TIMER)
// ---------------------------------------------------------------------------
void ChromeBrowser::TriggerCapture()
{
    if (!mWebViewInit || !mWebView) return;

    HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, 0);
    if (!hGlobal) return;

    ComPtr<IStream> stream;
    HRESULT hr = CreateStreamOnHGlobal(hGlobal, TRUE, &stream);
    if (FAILED(hr))
    {
        GlobalFree(hGlobal);
        return;
    }

    mWebView->CapturePreview(
        COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_DIB,
        stream.Get(),
        Callback<ICoreWebView2CapturePreviewCompletedHandler>(
            [this, stream](HRESULT res) -> HRESULT
            {
                if (FAILED(res)) return res;

                HGLOBAL hg = nullptr;
                if (FAILED(GetHGlobalFromStream(stream.Get(), &hg)))
                    return S_OK;

                size_t dataSize = GlobalSize(hg);
                const uint8_t* data =
                    (const uint8_t*)GlobalLock(hg);
                if (!data || dataSize < sizeof(BITMAPINFOHEADER))
                {
                    GlobalUnlock(hg);
                    return S_OK;
                }

                const BITMAPINFOHEADER* bih =
                    (const BITMAPINFOHEADER*)data;

                uint32_t w   = (uint32_t)bih->biWidth;
                uint32_t h   = (uint32_t)std::abs((int)bih->biHeight);
                uint16_t bpp = bih->biBitCount;

                if (bpp != 32 || w == 0 || h == 0)
                {
                    GlobalUnlock(hg);
                    return S_OK;
                }

                uint32_t dibStride = ((w * 32 + 31) / 32) * 4;
                uint32_t outStride = w * 4;

                const uint8_t* pixelData =
                    data + sizeof(BITMAPINFOHEADER);

                size_t expected = sizeof(BITMAPINFOHEADER) + dibStride * h;
                if (dataSize < expected)
                {
                    GlobalUnlock(hg);
                    return S_OK;
                }

                {
                    std::lock_guard<std::mutex> lk(mCaptureMutex);
                    mCaptured.pixels.resize(outStride * h);
                    mCaptured.w = w;
                    mCaptured.h = h;

                    bool topDown = (bih->biHeight < 0);
                    for (uint32_t y = 0; y < h; ++y)
                    {
                        uint32_t srcRow = topDown ? y : (h - 1 - y);
                        memcpy(mCaptured.pixels.data() + y * outStride,
                               pixelData + srcRow * dibStride,
                               outStride);
                    }
                    mCaptured.fresh = true;
                }

                GlobalUnlock(hg);
                return S_OK;
            }).Get());
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
        -1,-1, 0,0,
         1,-1, 1,0,
        -1, 1, 0,1,
         1, 1, 1,1
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
    uint32_t black = 0;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0,
                 GL_BGRA, GL_UNSIGNED_BYTE, &black);
    glBindTexture(GL_TEXTURE_2D, 0);

    mShaderReady = true;
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
        bool newVal = (value > 0.5f);
        if (newVal != mWindowVisible)
        {
            mShowWindow = value;
            ToggleWindow();
        }
        return FF_SUCCESS;
    }
    return FF_FAIL;
}

float ChromeBrowser::GetFloatParameter(unsigned int index)
{
    if (index == PARAM_SHOW_WINDOW)
        return mWindowVisible ? 1.0f : 0.0f;
    return 0.0f;
}

FFResult ChromeBrowser::SetTextParameter(unsigned int index, const char* value)
{
    if (index == PARAM_URL && value && value[0] != '\0')
    {
        mURL = value;
        MLog("URL set: " + mURL);
        mNeedsNav = true;
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
