# FFGL Chrome

FFGL plugins for [Resolume Arena](https://resolume.com/arena) built on the
[FFGL SDK](https://github.com/resolume/ffgl).

The plugin is **Chrome Browser** — it embeds a Chromium (WebView2) browser
inside Arena, so any web page can be used as a video source and routed into the
mix, the output, or a layer mask. A minimal test plugin is included to verify
that the SDK and toolchain work.

## Plugins

| Plugin | DLL | Type | ID | Description |
|---|---|---|---|---|
| Chrome Browser | `ChromeBrowser.dll` | Source | `CHRM` | Embedded WebView2 browser, captured and rendered as an OpenGL texture |
| Min Test | `MinTest.dll` | Effect | `MTST` | Outputs solid red — build/SDK smoke test |

## Chrome Browser

Parameters:

- **URL** (text) — page to load. A missing `://` is completed with `https://`.
- **Toggle Window** (event) — shows or hides the browser window so you can
  interact with it. The page keeps rendering while the window is hidden.
- **Go** (event) — navigates to the current URL value.

Toolbar buttons: back, forward, home.

The browser window is a normal top-level window owned by Arena. It is created
off-screen and, when shown, is centred on the primary display and brought to
the foreground; hiding it hands focus back to Arena. Closing the window hides it
rather than destroying it. Requests for new windows (`target=_blank`,
`window.open`) are redirected into the main WebView instead of opening separate
browser windows.

The user data folder (`chromebrowser_data`) is created next to the plugin DLL,
so cookies and logins survive restarts. If the plugin fails to load, check
`chromebrowser_dll.txt` next to the DLL.

**How frames are captured.** `ICoreWebView2::CapturePreview` returns a JPEG
snapshot, which is decoded with WIC to BGRA and uploaded to a GL texture. This
is a screen-capture path, not a shared-GPU-texture path, so the output updates
at the browser's capture rate rather than being a true per-frame video feed.
Cascading this plugin through a layer tree means high capture rates, high
browser resolution, or many instances will cost GPU and CPU.

## Requirements

- Windows 10/11 x64
- Resolume Arena 6.1 or 7 (FFGL API 2.1)
- Visual Studio 2019/2022 with the C++ desktop workload
- CMake 3.15 or newer
- [WebView2 Evergreen Runtime](https://developer.microsoft.com/microsoft-edge/webview2/)
- A clone of the FFGL SDK (`resolume/ffgl`)

## Building

```sh
git clone https://github.com/mcleon51/ffglchrome.git
cd ffglchrome

# FFGL SDK sources (required — not vendored here)
git clone https://github.com/resolume/ffgl.git third_party/ffgl

# WebView2 SDK (headers + loader only)
powershell -ExecutionPolicy Bypass -File src/deps/webview2/download_sdk.ps1

cmake -S src -B src/build -DFFGL_ROOT="$PWD/third_party/ffgl"
cmake --build src/build --config Release
```

If the WebView2 SDK is missing, CMake warns and skips `ChromeBrowser.dll`;
`MinTest.dll` still builds. `WEBVIEW2_ROOT` overrides SDK discovery (and
otherwise falls back to `find_package`, i.e. `vcpkg install webview2`).

Built DLLs land in `src/build/Release/`, with `WebView2Loader.dll` copied next
to them. Copy the plugins you want into your Arena `Plugins/ffgl` folder —
typically `%USERPROFILE%\Documents\Resolume Arena\Plugins\ffgl` — and they
appear in Arena on the next start.

## Repository layout

```
src/
  CMakeLists.txt                    build definitions
  deps/
    webview2/                       download_sdk.ps1 / .bat fetch the SDK
  source/
    plugins/ChromeBrowser/          the browser source plugin
    plugins/MinTest/                minimal SDK smoke test
```

FFGL SDK sources are compiled straight into each plugin DLL rather than linked
as a library, so `__declspec(dllexport)` on `plugMain` resolves correctly.

## Notes

- `ChromeBrowser.cpp` and `ChromeBrowser_new.cpp` in
  `src/source/plugins/ChromeBrowser/` are earlier drafts. Only
  `ChromeBrowser_fixed.cpp` is built; the other two are kept for reference and
  can be deleted.
- The plugin metadata in `ChromeBrowser_fixed.cpp:76` still credits the
  upstream repository the code was based on.

## License

MIT — see [LICENSE.md](LICENSE.md).
