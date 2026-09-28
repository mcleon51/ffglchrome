#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "ChromeBrowser.h"

#include <combaseapi.h>

#include <WebView2.h>
#include <wrl/client.h>
#include <wrl/event.h>

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

static CFFGLPluginInfo PluginInfo(
    PluginFactory< ChromeBrowser >,
    "CHRM", "Chrome Browser", 2, 1, 0, 0,
    FF_SOURCE,
    "Embeds a WebView2 browser. URL (text) + Show Window (toggle).",
    "github.com/odkkirova/ffglweb"
);