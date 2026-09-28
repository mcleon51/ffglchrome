#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// FFGL.h defines NOMSG (+ other NOxxx) which suppresses MSG/LPMSG in winuser.h.
// WebView2 COM headers require LPMSG, so undef NOMSG before the first
// windows.h include.  FFGL.h will redefine NOMSG later, but windows.h won't
// be reprocessed thanks to include guards, so MSG/LPMSG survive.
#undef NOMSG
#include <windows.h>

#include <wrl/client.h>
#include <wincodec.h>

#include <FFGLSDK.h>
#include <ffglex/FFGLShader.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

// Forward declarations for WebView2 COM types (full definitions in WebView2.h)
struct ICoreWebView2;
struct ICoreWebView2Controller;

// ---------------------------------------------------------------------------
// ChromeBrowser — FFGL Source plugin
//
// Embeds a WebView2 (Edge Chromium) browser, captures frames and renders
// them as an OpenGL texture.  Parameters:
//   URL (text)      – page to load
//   Show Window     – toggle (0=hidden, 1=visible) for user interaction
// ---------------------------------------------------------------------------

class ChromeBrowser : public CFFGLPlugin
{
public:
    ChromeBrowser();
    ~ChromeBrowser() override;

    FFResult InitGL( const FFGLViewportStruct* vp ) override;
    FFResult DeInitGL() override;
    FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;

    FFResult SetFloatParameter( unsigned int index, float value ) override;
    float    GetFloatParameter( unsigned int index ) override;
    FFResult SetTextParameter( unsigned int index, const char* value ) override;
    char*    GetTextParameter( unsigned int index ) override;

private:
    enum ParamIndex : unsigned int
    {
        PARAM_URL          = 0,
        PARAM_SHOW_WINDOW  = 1,
        PARAM_GO           = 2,
        PARAM_COUNT
    };

    std::string mURL           = "https://www.google.com";
    bool        mWindowVisible = false;

    // ---- WebView2 management (called from GL thread) ---------------------

    void CreateWebView();
    void DestroyWebView();
    void Navigate();
    void ToggleWindow();
    void TriggerCapture();
    void DecodeJpeg();

    static LRESULT CALLBACK WndProc( HWND hWnd, UINT msg,
                                     WPARAM wParam, LPARAM lParam );

    HWND   mHiddenWnd    = nullptr;
    HWND   mArenaHwnd    = nullptr;
    std::atomic<bool> mWebViewInit{false};
    bool   mNeedsNav     = false;
    bool   mWndClassRegd = false;
    bool   mComInitialized = false;

    // Toolbar
    HWND   mBtnBack    = nullptr;
    HWND   mBtnForward = nullptr;
    HWND   mBtnHome    = nullptr;
    static const int TOOLBAR_H = 32;

    Microsoft::WRL::ComPtr<ICoreWebView2Controller> mController;
    Microsoft::WRL::ComPtr<ICoreWebView2> mWebView;

    // ---- Frame capture ---------------------------------------------------

    // Populated by the CapturePreview callback (runs on the Win32 thread
    // that created the WebView, i.e. the GL / Resolume thread).
    struct CapturedFrame
    {
        uint32_t         w = 0, h = 0;
        std::vector<uint8_t> pixels;
        bool             fresh = false;
    };

    std::mutex   mCaptureMutex;
    CapturedFrame mCaptured;
    bool         mCapturePending = false;
    std::chrono::steady_clock::time_point mCapturePendingSince;
    bool         mNeedsFreshCapture = false;
    std::chrono::steady_clock::time_point mFreshCaptureAfter;
    Microsoft::WRL::ComPtr<IWICImagingFactory> mWICFactory;
    std::vector<uint8_t> mDecodeBuffer;

    // Pipeline: callback copies raw JPEG → ProcessOpenGL decodes + uploads
    std::vector<uint8_t> mJpegBuffer;
    bool                 mJpegReady = false;
    bool                 mNeedsTriggerCapture = false;

    // ---- GL resources ----------------------------------------------------

    ffglex::FFGLShader mShader;
    GLuint             mTexture      = 0;
    uint32_t           mTexW         = 0;
    uint32_t           mTexH         = 0;
    GLuint             mVAO          = 0;
    GLuint             mVBO          = 0;
    bool               mShaderReady  = false;

    // Staging buffer for pixel upload (held outside the mutex)
    std::vector<uint8_t> mUploadPixels;
    uint32_t             mUploadW = 0, mUploadH = 0;

    std::chrono::steady_clock::time_point mLastCaptureTime;
};
