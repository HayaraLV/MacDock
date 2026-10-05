// ============================================================
// DOCK.cpp
// ============================================================
//
// Windows 10 / Windows 11
// Visual Studio 2022 / 2026
// Windows SDK 10.0.26100.0+
// x64
// C++17
//
// macOS-style Dock
//
// Win32 + Direct2D + WIC + DWM
//
// ============================================================

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <windowsx.h>

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <dwmapi.h>

#include <d2d1.h>
#include <wincodec.h>
#include <wrl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

namespace fs = std::filesystem;


// ============================================================
// CONSTANTS
// ============================================================

static constexpr wchar_t WINDOW_CLASS[] = L"HayaraMacDockWindow";
static constexpr wchar_t WINDOW_TITLE[] = L"MacDock";
static constexpr UINT_PTR TIMER_ID = 1;

// DPI, к которому мы прибиваем Direct2D. 96 = 1 DIP == 1 физический
// пиксель. Это критично: WM_MOUSEMOVE приходит в физических пикселях,
// и при другом D2D DPI координаты рендера и мыши расходятся.
static constexpr float D2D_FIXED_DPI = 96.0f;


// ============================================================
// UTILITY
// ============================================================

static float ClampFloat(float value, float minimum, float maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}


static int ClampInt(int value, int minimum, int maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}


static std::wstring Trim(const std::wstring& value)
{
    const size_t first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return L"";
    const size_t last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}


static std::wstring Lower(std::wstring value)
{
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](wchar_t c)
        {
            return static_cast<wchar_t>(std::towlower(c));
        }
    );
    return value;
}


static bool IsValidPath(const std::wstring& path)
{
    if (path.empty()) return false;
    std::error_code ec;
    return fs::exists(fs::path(path), ec);
}


static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};

    const int len = WideCharToMultiByte(
        CP_UTF8, 0,
        w.c_str(), static_cast<int>(w.size()),
        nullptr, 0, nullptr, nullptr);

    if (len <= 0) return {};

    std::string out(static_cast<size_t>(len), '\0');

    WideCharToMultiByte(
        CP_UTF8, 0,
        w.c_str(), static_cast<int>(w.size()),
        out.data(), len, nullptr, nullptr);

    return out;
}


// ============================================================
// LOGGER
// ============================================================

class Logger
{
public:

    Logger() = default;

    ~Logger()
    {
        Close();
    }


    bool Open(const std::wstring& path)
    {
        Close();

        handle_ = CreateFileW(
            path.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );

        if (handle_ == INVALID_HANDLE_VALUE)
            return false;

        LARGE_INTEGER size{};

        if (GetFileSizeEx(handle_, &size) && size.QuadPart == 0)
        {
            const char bom[3] = { '\xEF', '\xBB', '\xBF' };
            DWORD written = 0;
            WriteFile(handle_, bom, 3, &written, nullptr);
        }

        return true;
    }


    void Close()
    {
        if (handle_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }


    template <typename... Args>
    void Info(Args&&... args)
    {
        Write(L"INFO", Build(args...));
    }

    template <typename... Args>
    void Warn(Args&&... args)
    {
        Write(L"WARN", Build(args...));
    }

    template <typename... Args>
    void Error(Args&&... args)
    {
        Write(L"ERROR", Build(args...));
    }


private:

    template <typename... Args>
    static std::wstring Build(Args&&... args)
    {
        std::wstringstream ss;
        (ss << ... << args);
        return ss.str();
    }


    void Write(const wchar_t* level, const std::wstring& message)
    {
        if (handle_ == INVALID_HANDLE_VALUE)
            return;

        SYSTEMTIME st{};
        GetLocalTime(&st);

        wchar_t prefix[128]{};

        swprintf_s(
            prefix,
            L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] [%-5s] ",
            static_cast<unsigned>(st.wYear),
            static_cast<unsigned>(st.wMonth),
            static_cast<unsigned>(st.wDay),
            static_cast<unsigned>(st.wHour),
            static_cast<unsigned>(st.wMinute),
            static_cast<unsigned>(st.wSecond),
            static_cast<unsigned>(st.wMilliseconds),
            level
        );

        std::wstring line = prefix;
        line += message;
        line += L"\r\n";

        const std::string utf8 = WideToUtf8(line);

        if (!utf8.empty())
        {
            DWORD written = 0;

            WriteFile(
                handle_,
                utf8.data(),
                static_cast<DWORD>(utf8.size()),
                &written,
                nullptr
            );

            FlushFileBuffers(handle_);
        }

        OutputDebugStringW(line.c_str());
    }


    HANDLE handle_ = INVALID_HANDLE_VALUE;
};


// ============================================================
// CONFIG WATCHER
// ============================================================

class ConfigWatcher
{
public:

    void Initialize(const std::wstring& path)
    {
        path_ = path;
        accumulatorMs_ = 0;
        stableTicks_ = 0;
        lastSeenValid_ = false;

        Refresh();
    }


    void Refresh()
    {
        WIN32_FILE_ATTRIBUTE_DATA data{};

        if (!GetFileAttributesExW(
            path_.c_str(), GetFileExInfoStandard, &data))
        {
            knownValid_ = false;
            return;
        }

        knownWrite_ = data.ftLastWriteTime;

        knownSize_ =
            (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) |
            static_cast<ULONGLONG>(data.nFileSizeLow);

        knownValid_ = true;

        stableTicks_ = 0;
        lastSeenValid_ = false;
    }


    bool Poll(DWORD deltaMs)
    {
        if (path_.empty())
            return false;

        accumulatorMs_ += deltaMs;

        if (accumulatorMs_ < checkIntervalMs_)
            return false;

        accumulatorMs_ = 0;

        WIN32_FILE_ATTRIBUTE_DATA data{};

        if (!GetFileAttributesExW(
            path_.c_str(), GetFileExInfoStandard, &data))
        {
            return false;
        }

        const ULONGLONG size =
            (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) |
            static_cast<ULONGLONG>(data.nFileSizeLow);

        const bool sameAsKnown =
            knownValid_ &&
            CompareFileTime(
                &data.ftLastWriteTime, &knownWrite_) == 0 &&
            size == knownSize_;

        if (sameAsKnown)
        {
            stableTicks_ = 0;
            lastSeenValid_ = false;
            return false;
        }

        const bool sameAsLastSeen =
            lastSeenValid_ &&
            CompareFileTime(
                &data.ftLastWriteTime, &lastSeenWrite_) == 0 &&
            size == lastSeenSize_;

        if (!sameAsLastSeen)
        {
            lastSeenWrite_ = data.ftLastWriteTime;
            lastSeenSize_ = size;
            lastSeenValid_ = true;
            stableTicks_ = 0;
            return false;
        }

        ++stableTicks_;

        if (stableTicks_ < stableTicksRequired_)
            return false;

        knownWrite_ = data.ftLastWriteTime;
        knownSize_ = size;
        knownValid_ = true;

        stableTicks_ = 0;
        lastSeenValid_ = false;

        return true;
    }


private:

    std::wstring path_;

    FILETIME knownWrite_{};
    ULONGLONG knownSize_ = 0;
    bool knownValid_ = false;

    FILETIME lastSeenWrite_{};
    ULONGLONG lastSeenSize_ = 0;
    bool lastSeenValid_ = false;

    DWORD accumulatorMs_ = 0;
    DWORD checkIntervalMs_ = 200;
    int stableTicks_ = 0;
    int stableTicksRequired_ = 2;
};


// ============================================================
// CONFIGURATION
// ============================================================

struct DockItem
{
    std::wstring path;
};


struct DockConfig
{
    int dockSizeX = 700;
    int dockSizeY = 110;

    int iconSize = 72;
    int iconSpacing = 8;

    int gridColumns = 0;
    int gridRows = 0;

    float transparencyLevel = 0.82f;
    float cornerRadius = 18.0f;
    bool blurEffect = true;

    int panelColorR = 9;
    int panelColorG = 9;
    int panelColorB = 12;

    int borderColorR = 255;
    int borderColorG = 255;
    int borderColorB = 255;
    float borderAlpha = 0.20f;

    float highlightAlpha = 0.14f;
    float glassTint = 0.08f;

    float animationSpeed = 1.0f;
    bool bounceEnabled = true;
    float bounceHeight = 18.0f;

    float magnificationScale = 1.75f;
    float magnificationRange = 130.0f;

    std::wstring edgePosition = L"bottom";
    int edgeMargin = 8;
    bool hideTaskbar = true;

    std::vector<DockItem> items;
};


// ============================================================
// CONFIG MANAGER
// ============================================================

class ConfigManager
{
public:

    explicit ConfigManager(std::wstring filename)
        : filename_(std::move(filename))
    {
    }


    DockConfig Load()
    {
        DockConfig config;

        std::wifstream file(filename_);

        if (!file.is_open())
        {
            Save(config);
            return config;
        }

        std::wstring line;

        while (std::getline(file, line))
        {
            line = Trim(line);

            if (line.empty()) continue;
            if (line[0] == L'#') continue;

            const size_t separator = line.find(L'=');

            if (separator == std::wstring::npos)
                continue;

            const std::wstring key =
                Lower(Trim(line.substr(0, separator)));

            const std::wstring value =
                Trim(line.substr(separator + 1));


            if (key == L"dock_size_x")
                config.dockSizeX = ClampInt(_wtoi(value.c_str()), 200, 4000);
            else if (key == L"dock_size_y")
                config.dockSizeY = ClampInt(_wtoi(value.c_str()), 60, 2000);

            else if (key == L"icon_size")
                config.iconSize = ClampInt(_wtoi(value.c_str()), 32, 256);
            else if (key == L"icon_spacing")
                config.iconSpacing = ClampInt(_wtoi(value.c_str()), 0, 100);

            else if (key == L"grid_columns")
                config.gridColumns = ClampInt(_wtoi(value.c_str()), 0, 64);
            else if (key == L"grid_rows")
                config.gridRows = ClampInt(_wtoi(value.c_str()), 0, 64);

            else if (key == L"transparency_level")
                config.transparencyLevel = ClampFloat(
                    static_cast<float>(_wtof(value.c_str())), 0.0f, 1.0f);
            else if (key == L"corner_radius")
                config.cornerRadius = ClampFloat(
                    static_cast<float>(_wtof(value.c_str())), 0.0f, 200.0f);

            else if (key == L"blur_effect")
            {
                const std::wstring v = Lower(value);
                config.blurEffect = (v == L"true" || v == L"1" || v == L"yes");
            }

            else if (key == L"panel_color_r")
                config.panelColorR = ClampInt(_wtoi(value.c_str()), 0, 255);
            else if (key == L"panel_color_g")
                config.panelColorG = ClampInt(_wtoi(value.c_str()), 0, 255);
            else if (key == L"panel_color_b")
                config.panelColorB = ClampInt(_wtoi(value.c_str()), 0, 255);

            else if (key == L"border_color_r")
                config.borderColorR = ClampInt(_wtoi(value.c_str()), 0, 255);
            else if (key == L"border_color_g")
                config.borderColorG = ClampInt(_wtoi(value.c_str()), 0, 255);
            else if (key == L"border_color_b")
                config.borderColorB = ClampInt(_wtoi(value.c_str()), 0, 255);

            else if (key == L"border_alpha")
                config.borderAlpha = ClampFloat(
                    static_cast<float>(_wtof(value.c_str())), 0.0f, 1.0f);
            else if (key == L"highlight_alpha")
                config.highlightAlpha = ClampFloat(
                    static_cast<float>(_wtof(value.c_str())), 0.0f, 1.0f);
            else if (key == L"glass_tint")
                config.glassTint = ClampFloat(
                    static_cast<float>(_wtof(value.c_str())), 0.0f, 1.0f);

            else if (key == L"animation_speed")
                config.animationSpeed = ClampFloat(
                    static_cast<float>(_wtof(value.c_str())), 0.05f, 5.0f);

            else if (key == L"bounce_enabled")
            {
                const std::wstring v = Lower(value);
                config.bounceEnabled = (v == L"true" || v == L"1" || v == L"yes");
            }
            else if (key == L"bounce_height")
                config.bounceHeight = ClampFloat(
                    static_cast<float>(_wtof(value.c_str())), 0.0f, 200.0f);

            else if (key == L"magnification_scale")
                config.magnificationScale = ClampFloat(
                    static_cast<float>(_wtof(value.c_str())), 1.0f, 4.0f);
            else if (key == L"magnification_range")
                config.magnificationRange = ClampFloat(
                    static_cast<float>(_wtof(value.c_str())), 10.0f, 800.0f);

            else if (key == L"edge_position")
            {
                const std::wstring v = Lower(value);

                if (v == L"bottom" || v == L"top" ||
                    v == L"left" || v == L"right")
                {
                    config.edgePosition = v;
                }
            }
            else if (key == L"edge_margin")
                config.edgeMargin = ClampInt(_wtoi(value.c_str()), 0, 400);

            else if (key == L"hide_taskbar")
            {
                const std::wstring v = Lower(value);
                config.hideTaskbar = (v == L"true" || v == L"1" || v == L"yes");
            }

            else if (key == L"item")
            {
                if (!value.empty())
                    config.items.push_back({ value });
            }
        }

        return config;
    }


    void Save(const DockConfig& config)
    {
        std::wofstream file(filename_, std::ios::trunc);

        if (!file.is_open())
            return;

        file
            << L"# ==================================================\n"
            << L"# Hayara MacDock configuration\n"
            << L"# ==================================================\n\n";

        file
            << L"# Dock size (pixels)\n"
            << L"dock_size_x=" << config.dockSizeX << L"\n"
            << L"dock_size_y=" << config.dockSizeY << L"\n\n";

        file
            << L"# Icon size / spacing\n"
            << L"icon_size=" << config.iconSize << L"\n"
            << L"icon_spacing=" << config.iconSpacing << L"\n\n";

        file
            << L"# Grid (0 = auto)\n"
            << L"grid_columns=" << config.gridColumns << L"\n"
            << L"grid_rows=" << config.gridRows << L"\n\n";

        file
            << L"# Panel look\n"
            << L"corner_radius=" << config.cornerRadius << L"\n"
            << L"transparency_level=" << config.transparencyLevel << L"\n"
            << L"glass_tint=" << config.glassTint << L"\n\n";

        file
            << L"# Panel color (0-255)\n"
            << L"panel_color_r=" << config.panelColorR << L"\n"
            << L"panel_color_g=" << config.panelColorG << L"\n"
            << L"panel_color_b=" << config.panelColorB << L"\n\n";

        file
            << L"# Border / highlight\n"
            << L"border_color_r=" << config.borderColorR << L"\n"
            << L"border_color_g=" << config.borderColorG << L"\n"
            << L"border_color_b=" << config.borderColorB << L"\n"
            << L"border_alpha=" << config.borderAlpha << L"\n"
            << L"highlight_alpha=" << config.highlightAlpha << L"\n\n";

        file
            << L"# DWM backdrop\n"
            << L"blur_effect="
            << (config.blurEffect ? L"true" : L"false")
            << L"\n\n";

        file
            << L"# Animation\n"
            << L"animation_speed=" << config.animationSpeed << L"\n"
            << L"bounce_enabled="
            << (config.bounceEnabled ? L"true" : L"false")
            << L"\n"
            << L"bounce_height=" << config.bounceHeight << L"\n\n";

        file
            << L"# Magnification\n"
            << L"magnification_scale=" << config.magnificationScale << L"\n"
            << L"magnification_range=" << config.magnificationRange << L"\n\n";

        file
            << L"# Placement\n"
            << L"edge_position=" << config.edgePosition << L"\n"
            << L"edge_margin=" << config.edgeMargin << L"\n"
            << L"hide_taskbar="
            << (config.hideTaskbar ? L"true" : L"false")
            << L"\n\n";

        file << L"# Dock items\n";

        for (const auto& item : config.items)
            file << L"item=" << item.path << L"\n";
    }


private:

    std::wstring filename_;
};


// ============================================================
// TASKBAR MANAGER
// ============================================================

class TaskbarManager
{
public:

    bool Hide()
    {
        taskbar_ = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (!taskbar_) return false;

        wasVisible_ = IsWindowVisible(taskbar_) != FALSE;

        if (wasVisible_)
        {
            ShowWindow(taskbar_, SW_HIDE);
            UpdateWindow(taskbar_);
        }
        return true;
    }


    void Restore()
    {
        if (!taskbar_ || !IsWindow(taskbar_))
            taskbar_ = FindWindowW(L"Shell_TrayWnd", nullptr);

        if (!taskbar_) return;

        if (wasVisible_)
        {
            ShowWindow(taskbar_, SW_SHOW);

            SetWindowPos(
                taskbar_, HWND_TOP, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE |
                SWP_NOACTIVATE | SWP_SHOWWINDOW
            );

            UpdateWindow(taskbar_);
        }
    }


private:

    HWND taskbar_ = nullptr;
    bool wasVisible_ = false;
};


// ============================================================
// RUNTIME ICON
// ============================================================

struct RuntimeIcon
{
    DockItem item;
    ComPtr<ID2D1Bitmap> bitmap;
    HICON icon = nullptr;

    float baseX = 0.0f;
    float baseY = 0.0f;
    float centerX = 0.0f;
    float centerY = 0.0f;

    float scale = 1.0f;
    float targetScale = 1.0f;
    float alpha = 1.0f;

    float bounceTime = 0.0f;
    bool removing = false;
};


// ============================================================
// DIRECT2D RENDERER
// ============================================================

class Renderer
{
public:

    bool Initialize(HWND hwnd)
    {
        hwnd_ = hwnd;

        ID2D1Factory* rawFactory = nullptr;

        HRESULT hr = D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED,
            &rawFactory
        );

        if (FAILED(hr)) return false;

        factory_.Attach(rawFactory);

        return CreateRenderTarget();
    }


    bool Resize(UINT width, UINT height)
    {
        if (!renderTarget_) return true;

        HRESULT hr = renderTarget_->Resize(D2D1::SizeU(width, height));

        if (hr == D2DERR_RECREATE_TARGET)
        {
            renderTarget_.Reset();
            return CreateRenderTarget();
        }

        return SUCCEEDED(hr);
    }


    void Begin()
    {
        if (renderTarget_)
            renderTarget_->BeginDraw();
    }


    HRESULT End()
    {
        if (!renderTarget_) return E_FAIL;

        HRESULT hr = renderTarget_->EndDraw();

        if (hr == D2DERR_RECREATE_TARGET)
        {
            renderTarget_.Reset();
            CreateRenderTarget();
        }

        return hr;
    }


    ID2D1RenderTarget* Target()
    {
        return renderTarget_.Get();
    }


private:

    bool CreateRenderTarget()
    {
        RECT rect{};
        GetClientRect(hwnd_, &rect);

        if (rect.right <= 0 || rect.bottom <= 0)
            return false;


        // КЛЮЧЕВОЙ МОМЕНТ:
        //
        // dpiX = dpiY = 96.0f — жёстко. Это заставляет Direct2D
        // работать 1:1 с физическими пикселями клиента, независимо
        // от системного масштаба Windows (100% / 125% / 150% / 175%).
        //
        // Без этого при масштабе != 100% D2D начинает рисовать в DIP,
        // а WM_MOUSEMOVE продолжает приходить в физических пикселях.
        // Рендер и хит-тест расходятся — мышь "промахивается".
        //
        const D2D1_RENDER_TARGET_PROPERTIES properties =
            D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(
                    DXGI_FORMAT_B8G8R8A8_UNORM,
                    D2D1_ALPHA_MODE_PREMULTIPLIED
                ),
                D2D_FIXED_DPI,
                D2D_FIXED_DPI,
                D2D1_RENDER_TARGET_USAGE_NONE,
                D2D1_FEATURE_LEVEL_DEFAULT
            );


        const D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProperties =
            D2D1::HwndRenderTargetProperties(
                hwnd_,
                D2D1::SizeU(
                    static_cast<UINT>(rect.right),
                    static_cast<UINT>(rect.bottom)
                ),
                D2D1_PRESENT_OPTIONS_NONE
            );


        ID2D1HwndRenderTarget* rawTarget = nullptr;

        HRESULT hr = factory_->CreateHwndRenderTarget(
            properties,
            hwndProperties,
            &rawTarget
        );

        if (FAILED(hr))
            return false;

        renderTarget_.Attach(rawTarget);


        // Дублируем DPI прямо на таргете. Некоторые версии D2D
        // игнорируют DPI из properties и берут DPI окна —
        // SetDpi() прибивает окончательно.
        renderTarget_->SetDpi(D2D_FIXED_DPI, D2D_FIXED_DPI);


        return true;
    }


    HWND hwnd_ = nullptr;

    ComPtr<ID2D1Factory> factory_;
    ComPtr<ID2D1HwndRenderTarget> renderTarget_;
};


// ============================================================
// MAIN APPLICATION
// ============================================================

class DockApplication
{
public:

    DockApplication()
        : configManager_(GetConfigPath())
    {
    }


    ~DockApplication()
    {
        Shutdown();
    }


    bool Initialize(HINSTANCE instance)
    {
        instance_ = instance;

        if (logger_.Open(GetLogPath()))
        {
            logger_.Info(L"========== Dock starting ==========");
        }

        SetProcessDpiAwarenessContext(
            DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        );


        config_ = configManager_.Load();

        logger_.Info(
            L"Config loaded: items=", static_cast<int>(config_.items.size()),
            L" edge=", config_.edgePosition,
            L" size=", config_.dockSizeX, L"x", config_.dockSizeY,
            L" icon=", config_.iconSize
        );


        configWatcher_.Initialize(GetConfigPath());


        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.hInstance = instance_;
        wc.lpfnWndProc = WindowProcStatic;
        wc.lpszClassName = WINDOW_CLASS;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.style = CS_HREDRAW | CS_VREDRAW;


        if (!RegisterClassExW(&wc))
        {
            if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            {
                logger_.Error(L"RegisterClassExW failed");
                return false;
            }
        }


        hwnd_ = CreateWindowExW(
            WS_EX_TOOLWINDOW |
            WS_EX_TOPMOST |
            WS_EX_NOACTIVATE |
            WS_EX_ACCEPTFILES,

            WINDOW_CLASS,
            WINDOW_TITLE,

            WS_POPUP,

            0, 0,
            config_.dockSizeX,
            config_.dockSizeY,

            nullptr, nullptr,
            instance_, this
        );


        if (!hwnd_)
        {
            logger_.Error(L"CreateWindowExW failed");
            return false;
        }


        DragAcceptFiles(hwnd_, TRUE);

        renderer_ = std::make_unique<Renderer>();

        if (!renderer_->Initialize(hwnd_))
        {
            logger_.Error(L"Renderer init failed");
            return false;
        }

        if (!renderer_->Target())
        {
            logger_.Error(L"Renderer target missing");
            return false;
        }


        ApplyDwmStyle();


        if (config_.hideTaskbar)
        {
            const bool ok = taskbar_.Hide();
            logger_.Info(L"Taskbar hide: ", ok ? L"ok" : L"failed");
        }


        LoadIcons();
        UpdateLayout();


        // Логируем фактические размеры — если D2D и окно расходятся,
        // тут это сразу видно.
        LogSizes(L"after-init");


        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        UpdateWindow(hwnd_);


        SetTimer(hwnd_, TIMER_ID, 8, nullptr);
        lastFrameTime_ = std::chrono::steady_clock::now();


        const UINT windowDpi = GetDpiForWindow(hwnd_);

        logger_.Info(
            L"Window DPI (informational): ", static_cast<int>(windowDpi),
            L", D2D DPI pinned to: ", static_cast<int>(D2D_FIXED_DPI)
        );


        initialized_ = true;
        logger_.Info(L"Dock started");

        return true;
    }


    int Run()
    {
        MSG msg{};

        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        return static_cast<int>(msg.wParam);
    }


    void Shutdown()
    {
        if (shutdown_) return;
        shutdown_ = true;

        logger_.Info(L"Dock shutting down");

        if (hwnd_) KillTimer(hwnd_, TIMER_ID);

        SaveConfigInternal(L"shutdown");
        ClearIcons();

        renderer_.reset();
        taskbar_.Restore();

        if (hwnd_ && IsWindow(hwnd_))
        {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }

        logger_.Info(L"Dock stopped");
        logger_.Close();
    }


private:

    static std::wstring GetConfigPath()
    {
        wchar_t buffer[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        if (length == 0) return L"dock_settings.cfg";
        fs::path exePath(std::wstring(buffer, length));
        return (exePath.parent_path() / L"dock_settings.cfg").wstring();
    }


    static std::wstring GetLogPath()
    {
        wchar_t buffer[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        if (length == 0) return L"dock.log";
        fs::path exePath(std::wstring(buffer, length));
        return (exePath.parent_path() / L"dock.log").wstring();
    }


    void SaveConfigInternal(const wchar_t* reason)
    {
        configManager_.Save(config_);
        configWatcher_.Refresh();

        if (reason)
        {
            logger_.Info(
                L"Config saved (", reason, L"), items=",
                static_cast<int>(config_.items.size())
            );
        }
    }


    void LogSizes(const wchar_t* tag)
    {
        if (!renderer_ || !renderer_->Target())
            return;

        const D2D1_SIZE_F dip = renderer_->Target()->GetSize();

        RECT client{};
        GetClientRect(hwnd_, &client);

        logger_.Info(
            L"Sizes[", tag, L"]: window=",
            windowWidth_, L"x", windowHeight_,
            L" client=", (client.right - client.left),
            L"x", (client.bottom - client.top),
            L" d2d=", static_cast<int>(dip.width),
            L"x", static_cast<int>(dip.height)
        );
    }


    // ========================================================
    // WINDOW PROC
    // ========================================================

    static LRESULT CALLBACK WindowProcStatic(
        HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        DockApplication* app =
            reinterpret_cast<DockApplication*>(
                GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        if (message == WM_NCCREATE)
        {
            auto* createStruct =
                reinterpret_cast<CREATESTRUCTW*>(lParam);

            app = static_cast<DockApplication*>(
                createStruct->lpCreateParams);

            SetWindowLongPtrW(
                hwnd, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(app));
        }

        if (app)
            return app->WindowProc(hwnd, message, wParam, lParam);

        return DefWindowProcW(hwnd, message, wParam, lParam);
    }


    LRESULT WindowProc(
        HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_ERASEBKGND:
            return 1;


        case WM_PAINT:
        {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            Render();
            EndPaint(hwnd, &ps);
            return 0;
        }


        case WM_TIMER:
        {
            if (wParam == TIMER_ID)
            {
                const auto now = std::chrono::steady_clock::now();

                float dt = std::chrono::duration<float>(
                    now - lastFrameTime_).count();

                lastFrameTime_ = now;
                dt = ClampFloat(dt, 0.001f, 0.05f);

                UpdateAnimation(dt);

                const DWORD dtMs = static_cast<DWORD>(dt * 1000.0f);

                if (configWatcher_.Poll(dtMs))
                    ReloadConfig();

                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }


        case WM_MOUSEMOVE:
        {
            OnMouseMove(
                GET_X_LPARAM(lParam),
                GET_Y_LPARAM(lParam)
            );
            return 0;
        }


        case WM_MOUSELEAVE:
        {
            mouseInside_ = false;
            hoverX_ = -100000.0f;
            hoverY_ = -100000.0f;

            for (auto& icon : icons_)
                icon.targetScale = 1.0f;

            return 0;
        }


        case WM_LBUTTONDOWN:
        {
            leftButtonDown_ = true;

            mouseDownX_ = GET_X_LPARAM(lParam);
            mouseDownY_ = GET_Y_LPARAM(lParam);

            mouseDownIcon_ = HitTest(mouseDownX_, mouseDownY_);

            logger_.Info(
                L"LButtonDown at (", mouseDownX_, L",", mouseDownY_,
                L") -> icon=", mouseDownIcon_
            );

            if (mouseDownIcon_ >= 0)
                SetCapture(hwnd);

            return 0;
        }


        case WM_LBUTTONUP:
        {
            const bool wasDragging = internalDragging_;
            const int iconAtDown = mouseDownIcon_;

            internalDragging_ = false;
            leftButtonDown_ = false;
            mouseDownIcon_ = -1;

            if (GetCapture() == hwnd)
                ReleaseCapture();

            logger_.Info(
                L"LButtonUp: wasDragging=", wasDragging ? 1 : 0,
                L" iconAtDown=", iconAtDown
            );

            if (!wasDragging && iconAtDown >= 0)
                LaunchIcon(iconAtDown);

            return 0;
        }


        case WM_RBUTTONUP:
        {
            OnRightClick(
                GET_X_LPARAM(lParam),
                GET_Y_LPARAM(lParam)
            );
            return 0;
        }


        case WM_DROPFILES:
        {
            HandleDrop(reinterpret_cast<HDROP>(wParam));
            return 0;
        }


        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE:
        {
            logger_.Info(L"System change, re-layout");
            UpdateLayout();
            LogSizes(L"after-systemchange");
            return 0;
        }


        case WM_DPICHANGED:
        {
            const UINT newDpi = HIWORD(wParam);

            logger_.Info(
                L"DPI changed -> ", static_cast<int>(newDpi),
                L", keeping D2D pinned to 96"
            );

            UpdateLayout();
            LogSizes(L"after-dpichange");

            return 0;
        }


        case WM_CLOSE:
            Shutdown();
            PostQuitMessage(0);
            return 0;


        case WM_ENDSESSION:
            if (wParam) taskbar_.Restore();
            return 0;


        case WM_DESTROY:
            KillTimer(hwnd, TIMER_ID);
            return 0;
        }

        return DefWindowProcW(hwnd, message, wParam, lParam);
    }


    void ApplyDwmStyle()
    {
        constexpr DWORD DWMWA_WINDOW_CORNER_PREFERENCE_LOCAL = 33;
        constexpr int DWMWCP_ROUND_LOCAL = 2;

        DwmSetWindowAttribute(
            hwnd_,
            DWMWA_WINDOW_CORNER_PREFERENCE_LOCAL,
            &DWMWCP_ROUND_LOCAL,
            sizeof(DWMWCP_ROUND_LOCAL)
        );

        if (!config_.blurEffect) return;

        constexpr DWORD DWMWA_SYSTEMBACKDROP_TYPE_LOCAL = 38;
        constexpr int DWMSBT_TRANSIENTWINDOW_LOCAL = 3;

        DwmSetWindowAttribute(
            hwnd_,
            DWMWA_SYSTEMBACKDROP_TYPE_LOCAL,
            &DWMSBT_TRANSIENTWINDOW_LOCAL,
            sizeof(DWMSBT_TRANSIENTWINDOW_LOCAL)
        );
    }


    void ReloadConfig()
    {
        logger_.Info(L"Config file changed, reloading");

        const DockConfig newConfig = configManager_.Load();

        const int oldItems = static_cast<int>(config_.items.size());
        const int newItems = static_cast<int>(newConfig.items.size());

        config_ = newConfig;

        ApplyDwmStyle();

        if (config_.hideTaskbar)
            taskbar_.Hide();
        else
            taskbar_.Restore();

        LoadIcons();
        UpdateLayout();

        logger_.Info(
            L"Config reloaded: items ", oldItems, L" -> ", newItems,
            L" edge=", config_.edgePosition,
            L" size=", config_.dockSizeX, L"x", config_.dockSizeY
        );

        LogSizes(L"after-reload");
    }


    // ========================================================
    // LAYOUT
    // ========================================================

    int DockWidth() const { return ClampInt(config_.dockSizeX, 200, 4000); }
    int DockHeight() const { return ClampInt(config_.dockSizeY, 60, 2000); }
    int IconSize() const { return ClampInt(config_.iconSize, 32, 256); }

    bool IsHorizontal() const
    {
        return config_.edgePosition == L"bottom" ||
            config_.edgePosition == L"top";
    }


    void ComputeGrid()
    {
        const int count = static_cast<int>(icons_.size());

        if (count <= 0)
        {
            gridColumns_ = 1;
            gridRows_ = 1;
            return;
        }

        const int iconSize = IconSize();
        const int spacing = config_.iconSpacing;
        constexpr int padding = 10;

        const bool horizontal = IsHorizontal();

        const int availW = std::max(1, DockWidth() - padding * 2);
        const int availH = std::max(1, DockHeight() - padding * 2);

        int maxColsByW = (availW + spacing) / (iconSize + spacing);
        int maxRowsByH = (availH + spacing) / (iconSize + spacing);

        if (maxColsByW < 1) maxColsByW = 1;
        if (maxRowsByH < 1) maxRowsByH = 1;

        int cols = 1;
        int rows = 1;

        if (config_.gridColumns > 0 && config_.gridRows > 0)
        {
            cols = config_.gridColumns;
            rows = config_.gridRows;
        }
        else if (config_.gridColumns > 0)
        {
            cols = config_.gridColumns;
            rows = (count + cols - 1) / cols;
        }
        else if (config_.gridRows > 0)
        {
            rows = config_.gridRows;
            cols = (count + rows - 1) / rows;
        }
        else
        {
            if (horizontal)
            {
                cols = count;
                rows = 1;

                if (cols > maxColsByW)
                {
                    cols = maxColsByW;
                    rows = (count + cols - 1) / cols;
                }
            }
            else
            {
                rows = count;
                cols = 1;

                if (rows > maxRowsByH)
                {
                    rows = maxRowsByH;
                    cols = (count + rows - 1) / rows;
                }
            }
        }

        if (cols < 1) cols = 1;
        if (rows < 1) rows = 1;

        while (cols * rows < count)
        {
            if (horizontal) ++cols;
            else            ++rows;
        }

        if (cols > 64) cols = 64;
        if (rows > 64) rows = 64;

        gridColumns_ = cols;
        gridRows_ = rows;
    }


    void UpdateLayout()
    {
        ComputeGrid();

        const int iconSize = IconSize();
        const int spacing = config_.iconSpacing;
        constexpr int padding = 10;

        const int neededW =
            gridColumns_ * iconSize +
            (gridColumns_ - 1) * spacing +
            padding * 2;

        const int neededH =
            gridRows_ * iconSize +
            (gridRows_ - 1) * spacing +
            padding * 2;

        HMONITOR monitor =
            MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);

        if (!monitor) return;

        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);

        if (!GetMonitorInfoW(monitor, &monitorInfo))
            return;

        const RECT area = monitorInfo.rcMonitor;
        const int screenWidth = area.right - area.left;
        const int screenHeight = area.bottom - area.top;

        int width = std::max(DockWidth(), neededW);
        int height = std::max(DockHeight(), neededH);

        width = std::min(width, screenWidth);
        height = std::min(height, screenHeight);

        windowWidth_ = width;
        windowHeight_ = height;

        const int margin = config_.edgeMargin;

        int x = 0, y = 0;

        if (config_.edgePosition == L"bottom")
        {
            x = area.left + (screenWidth - width) / 2;
            y = area.bottom - height - margin;
        }
        else if (config_.edgePosition == L"top")
        {
            x = area.left + (screenWidth - width) / 2;
            y = area.top + margin;
        }
        else if (config_.edgePosition == L"left")
        {
            x = area.left + margin;
            y = area.top + (screenHeight - height) / 2;
        }
        else
        {
            x = area.right - width - margin;
            y = area.top + (screenHeight - height) / 2;
        }

        const int minX = area.left;
        const int maxX = area.right - width;
        x = ClampInt(x, minX, std::max(minX, maxX));

        const int minY = area.top;
        const int maxY = area.bottom - height;
        y = ClampInt(y, minY, std::max(minY, maxY));

        SetWindowPos(
            hwnd_, HWND_TOPMOST,
            x, y, width, height,
            SWP_NOACTIVATE
        );

        if (renderer_)
        {
            renderer_->Resize(
                static_cast<UINT>(width),
                static_cast<UINT>(height)
            );
        }

        RecalculatePositions();
    }


    void RecalculatePositions()
    {
        if (icons_.empty()) return;

        const int count = static_cast<int>(icons_.size());

        const float iconSize = static_cast<float>(IconSize());
        const float spacing = static_cast<float>(config_.iconSpacing);

        const float dockW = static_cast<float>(
            windowWidth_ > 0 ? windowWidth_ : DockWidth());

        const float dockH = static_cast<float>(
            windowHeight_ > 0 ? windowHeight_ : DockHeight());

        const int cols = std::max(1, gridColumns_);
        const int rows = std::max(1, gridRows_);

        const float gridW = cols * iconSize + (cols - 1) * spacing;
        const float gridH = rows * iconSize + (rows - 1) * spacing;

        const float cellW = iconSize + spacing;
        const float cellH = iconSize + spacing;

        const float startX = (dockW - gridW) * 0.5f + iconSize * 0.5f;
        const float startY = (dockH - gridH) * 0.5f + iconSize * 0.5f;

        const bool horizontal = IsHorizontal();
        const float bounceH = config_.bounceHeight;

        for (int i = 0; i < count; ++i)
        {
            RuntimeIcon& icon = icons_[i];

            int col = 0, row = 0;

            if (horizontal) { col = i % cols; row = i / cols; }
            else { row = i % rows; col = i / rows; }

            icon.baseX = startX + col * cellW;
            icon.baseY = startY + row * cellH;

            const float drawnSize = iconSize * icon.scale;
            const float delta = (drawnSize - iconSize) * 0.5f;

            float offsetX = 0.0f, offsetY = 0.0f;

            if (config_.edgePosition == L"bottom") offsetY = -delta;
            else if (config_.edgePosition == L"top") offsetY = delta;
            else if (config_.edgePosition == L"left") offsetX = delta;
            else offsetX = -delta;

            float bounceX = 0.0f, bounceY = 0.0f;

            if (config_.bounceEnabled && icon.bounceTime > 0.0f)
            {
                const float t = icon.bounceTime;

                const float bounce =
                    std::sin(t * 3.14159265358979323846f * 5.0f) *
                    bounceH * t;

                if (config_.edgePosition == L"bottom") bounceY = -bounce;
                else if (config_.edgePosition == L"top") bounceY = bounce;
                else if (config_.edgePosition == L"left") bounceX = bounce;
                else bounceX = -bounce;
            }

            icon.centerX = icon.baseX + offsetX + bounceX;
            icon.centerY = icon.baseY + offsetY + bounceY;
        }
    }


    // ========================================================
    // ICONS
    // ========================================================

    void ClearIcons()
    {
        for (auto& icon : icons_)
        {
            if (icon.icon)
            {
                DestroyIcon(icon.icon);
                icon.icon = nullptr;
            }
        }
        icons_.clear();
    }


    void LoadIcons()
    {
        ClearIcons();

        if (!renderer_ || !renderer_->Target())
        {
            logger_.Warn(L"LoadIcons: no renderer");
            return;
        }

        ComPtr<IWICImagingFactory> wicFactory;

        HRESULT hr = CoCreateInstance(
            CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory));

        if (FAILED(hr))
        {
            logger_.Error(L"LoadIcons: WIC factory failed");
            return;
        }

        int loaded = 0;
        int skippedInvalid = 0;
        int skippedIconFail = 0;

        for (const auto& item : config_.items)
        {
            if (!IsValidPath(item.path))
            {
                ++skippedInvalid;
                logger_.Warn(L"Icon skipped (bad path): ", item.path);
                continue;
            }

            SHFILEINFOW info{};

            if (!SHGetFileInfoW(
                item.path.c_str(), 0, &info, sizeof(info),
                SHGFI_ICON | SHGFI_LARGEICON))
            {
                ++skippedIconFail;
                logger_.Warn(L"Icon skipped (SHGetFileInfo): ", item.path);
                continue;
            }

            HICON hIcon = info.hIcon;

            if (!hIcon)
            {
                ++skippedIconFail;
                continue;
            }

            ComPtr<IWICBitmap> wicBitmap;

            hr = wicFactory->CreateBitmapFromHICON(hIcon, &wicBitmap);

            if (FAILED(hr)) { DestroyIcon(hIcon); ++skippedIconFail; continue; }

            ComPtr<IWICFormatConverter> converter;

            hr = wicFactory->CreateFormatConverter(&converter);

            if (FAILED(hr)) { DestroyIcon(hIcon); ++skippedIconFail; continue; }

            hr = converter->Initialize(
                wicBitmap.Get(),
                GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0,
                WICBitmapPaletteTypeMedianCut);

            if (FAILED(hr)) { DestroyIcon(hIcon); ++skippedIconFail; continue; }

            const D2D1_BITMAP_PROPERTIES bitmapProperties =
                D2D1::BitmapProperties(
                    D2D1::PixelFormat(
                        DXGI_FORMAT_B8G8R8A8_UNORM,
                        D2D1_ALPHA_MODE_PREMULTIPLIED),
                    D2D_FIXED_DPI,
                    D2D_FIXED_DPI
                );

            ComPtr<ID2D1Bitmap> bitmap;

            hr = renderer_->Target()->CreateBitmapFromWicBitmap(
                converter.Get(), &bitmapProperties, &bitmap);

            if (FAILED(hr)) { DestroyIcon(hIcon); ++skippedIconFail; continue; }

            RuntimeIcon runtime;
            runtime.item = item;
            runtime.bitmap = bitmap;
            runtime.icon = hIcon;

            icons_.push_back(std::move(runtime));
            ++loaded;
        }

        logger_.Info(
            L"LoadIcons: loaded=", loaded,
            L", skipped(invalid)=", skippedInvalid,
            L", skipped(other)=", skippedIconFail,
            L", config items=", static_cast<int>(config_.items.size())
        );
    }


    // ========================================================
    // MOUSE
    // ========================================================

    void OnMouseMove(int x, int y)
    {
        mouseInside_ = true;
        hoverX_ = static_cast<float>(x);
        hoverY_ = static_cast<float>(y);

        TRACKMOUSEEVENT event{};
        event.cbSize = sizeof(event);
        event.dwFlags = TME_LEAVE;
        event.hwndTrack = hwnd_;

        TrackMouseEvent(&event);

        if (leftButtonDown_ && mouseDownIcon_ >= 0)
        {
            const int dx = x - mouseDownX_;
            const int dy = y - mouseDownY_;

            if (std::abs(dx) > 8 || std::abs(dy) > 8)
            {
                internalDragging_ = true;

                RECT dockRect = GetDockRectScreen();

                POINT screenPoint{ x, y };
                ClientToScreen(hwnd_, &screenPoint);

                if (!PtInRect(&dockRect, screenPoint))
                    BeginRemoveAnimation(mouseDownIcon_);
            }
        }

        UpdateMagnification();
    }


    void UpdateMagnification()
    {
        if (!mouseInside_)
        {
            for (auto& icon : icons_)
                icon.targetScale = 1.0f;

            return;
        }

        const float range = std::max(1.0f, config_.magnificationRange);
        const float maxScale = std::max(1.0f, config_.magnificationScale);

        const bool horizontal = IsHorizontal();

        const bool gridMode =
            (horizontal && gridRows_ > 1) ||
            (!horizontal && gridColumns_ > 1);

        for (auto& icon : icons_)
        {
            const float dx = hoverX_ - icon.baseX;
            const float dy = hoverY_ - icon.baseY;

            float distance;

            if (gridMode)
                distance = std::sqrt(dx * dx + dy * dy);
            else
                distance = horizontal ? std::abs(dx) : std::abs(dy);

            const float normalized = ClampFloat(distance / range, 0.0f, 1.0f);
            const float gaussian = std::exp(-3.0f * normalized * normalized);

            icon.targetScale = 1.0f + gaussian * (maxScale - 1.0f);
        }
    }


    // ========================================================
    // ANIMATION
    // ========================================================

    void UpdateAnimation(float deltaSeconds)
    {
        const float dt = deltaSeconds * config_.animationSpeed;

        for (auto& icon : icons_)
        {
            const float smoothing = 1.0f - std::exp(-18.0f * dt);

            icon.scale += (icon.targetScale - icon.scale) * smoothing;

            if (icon.bounceTime > 0.0f)
            {
                icon.bounceTime -= dt * 1.7f;
                if (icon.bounceTime < 0.0f) icon.bounceTime = 0.0f;
            }

            if (icon.removing)
            {
                icon.alpha -= dt * 5.0f;
                icon.scale += (0.15f - icon.scale) * smoothing;
                if (icon.alpha < 0.0f) icon.alpha = 0.0f;
            }
        }

        RemoveFinishedIcons();
        RecalculatePositions();
    }


    void BeginRemoveAnimation(int index)
    {
        if (index < 0 || index >= static_cast<int>(icons_.size()))
            return;

        RuntimeIcon& icon = icons_[index];

        if (icon.removing) return;

        logger_.Info(L"Removal requested: ", icon.item.path);

        icon.removing = true;
        icon.targetScale = 0.15f;
    }


    void RemoveFinishedIcons()
    {
        bool changed = false;

        for (size_t i = 0; i < icons_.size(); )
        {
            if (icons_[i].removing && icons_[i].alpha <= 0.0f)
            {
                const std::wstring path = icons_[i].item.path;

                if (icons_[i].icon)
                {
                    DestroyIcon(icons_[i].icon);
                    icons_[i].icon = nullptr;
                }

                icons_.erase(
                    icons_.begin() + static_cast<std::ptrdiff_t>(i));

                const std::wstring lowered = Lower(path);

                config_.items.erase(
                    std::remove_if(
                        config_.items.begin(), config_.items.end(),
                        [&](const DockItem& item)
                        { return Lower(item.path) == lowered; }),
                    config_.items.end()
                );

                logger_.Info(L"Removed from dock: ", path);
                changed = true;
            }
            else
            {
                ++i;
            }
        }

        if (changed)
        {
            SaveConfigInternal(L"remove");
            UpdateLayout();
        }
    }


    // ========================================================
    // HIT TEST — по ячейкам сетки
    // ========================================================
    //
    // Никакого scale/магнификации. Клик в ячейку → иконка ячейки.
    // Совпадает с тем, что видно на экране.
    //
    // ========================================================

    int HitTest(int x, int y) const
    {
        if (icons_.empty() || gridColumns_ < 1 || gridRows_ < 1)
            return -1;

        const float iconSize = static_cast<float>(IconSize());
        const float spacing = static_cast<float>(config_.iconSpacing);

        const float cellW = iconSize + spacing;
        const float cellH = iconSize + spacing;

        const float gridW =
            gridColumns_ * iconSize + (gridColumns_ - 1) * spacing;
        const float gridH =
            gridRows_ * iconSize + (gridRows_ - 1) * spacing;

        const float startX =
            (static_cast<float>(windowWidth_) - gridW) * 0.5f;
        const float startY =
            (static_cast<float>(windowHeight_) - gridH) * 0.5f;

        const float localX = static_cast<float>(x) - startX;
        const float localY = static_cast<float>(y) - startY;

        if (localX < 0.0f || localY < 0.0f)
            return -1;

        int col = static_cast<int>(localX / cellW);
        int row = static_cast<int>(localY / cellH);

        if (col < 0) col = 0;
        if (row < 0) row = 0;
        if (col >= gridColumns_) col = gridColumns_ - 1;
        if (row >= gridRows_)    row = gridRows_ - 1;

        const bool horizontal = IsHorizontal();

        int index;

        if (horizontal)
            index = row * gridColumns_ + col;
        else
            index = col * gridRows_ + row;

        if (index < 0 || index >= static_cast<int>(icons_.size()))
            return -1;

        return index;
    }


    RECT GetDockRectScreen() const
    {
        RECT rect{};
        GetWindowRect(hwnd_, &rect);
        return rect;
    }


    // ========================================================
    // LAUNCH
    // ========================================================

    void LaunchIcon(int index)
    {
        if (index < 0 || index >= static_cast<int>(icons_.size()))
            return;

        RuntimeIcon& icon = icons_[index];

        if (icon.removing) return;

        icon.bounceTime = 1.0f;

        logger_.Info(L"Launch: ", icon.item.path);

        const HINSTANCE result = ShellExecuteW(
            nullptr, L"open",
            icon.item.path.c_str(),
            nullptr, nullptr,
            SW_SHOWNORMAL
        );

        const INT_PTR code = reinterpret_cast<INT_PTR>(result);

        if (code <= 32)
        {
            logger_.Error(
                L"ShellExecuteW failed, code=", static_cast<int>(code),
                L" path=", icon.item.path
            );
        }
        else
        {
            logger_.Info(L"ShellExecuteW ok");
        }
    }


    void OnRightClick(int x, int y)
    {
        const int index = HitTest(x, y);

        logger_.Info(L"RButtonUp at (", x, L",", y, L") -> icon=", index);

        if (index < 0) return;

        POINT point{ x, y };
        ClientToScreen(hwnd_, &point);

        HMENU menu = CreatePopupMenu();

        if (!menu) return;

        AppendMenuW(menu, MF_STRING, 1001, L"Remove from Dock");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 1002, L"Open configuration");
        AppendMenuW(menu, MF_STRING, 1003, L"Open log");

        const UINT command = TrackPopupMenu(
            menu,
            TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
            point.x, point.y, 0,
            hwnd_, nullptr
        );

        DestroyMenu(menu);

        if (command == 1001)
        {
            BeginRemoveAnimation(index);
        }
        else if (command == 1002)
        {
            ShellExecuteW(nullptr, L"open", GetConfigPath().c_str(),
                nullptr, nullptr, SW_SHOWNORMAL);
        }
        else if (command == 1003)
        {
            ShellExecuteW(nullptr, L"open", GetLogPath().c_str(),
                nullptr, nullptr, SW_SHOWNORMAL);
        }
    }


    void HandleDrop(HDROP drop)
    {
        if (!drop) return;

        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);

        bool changed = false;

        for (UINT i = 0; i < count; ++i)
        {
            wchar_t path[MAX_PATH]{};

            if (DragQueryFileW(drop, i, path, MAX_PATH) == 0)
                continue;

            const std::wstring file = path;

            const std::wstring extension =
                Lower(fs::path(file).extension().wstring());

            if (extension != L".exe" && extension != L".lnk")
            {
                logger_.Warn(L"Drop skipped (not exe/lnk): ", file);
                continue;
            }

            const std::wstring lowered = Lower(file);

            bool exists = false;

            for (const auto& item : config_.items)
            {
                if (Lower(item.path) == lowered)
                {
                    exists = true;
                    break;
                }
            }

            if (!exists)
            {
                config_.items.push_back({ file });
                changed = true;

                logger_.Info(L"Drop added: ", file);
            }
            else
            {
                logger_.Info(L"Drop ignored (already in dock): ", file);
            }
        }

        DragFinish(drop);

        if (changed)
        {
            SaveConfigInternal(L"drop");
            LoadIcons();
            UpdateLayout();
        }
    }


    // ========================================================
    // RENDERING
    // ========================================================
    //
    // ВАЖНО: используем windowWidth_/windowHeight_, а не
    // target->GetSize(). При DPI 96 они совпадают, но так мы
    // явно рисуем в той же системе координат, что и хит-тест.
    //
    // ========================================================

    void Render()
    {
        if (!renderer_) return;

        ID2D1RenderTarget* target = renderer_->Target();

        if (!target) return;

        const float W = static_cast<float>(
            windowWidth_ > 0 ? windowWidth_ : DockWidth());
        const float H = static_cast<float>(
            windowHeight_ > 0 ? windowHeight_ : DockHeight());

        // Синхронизируем позиции прямо перед рисованием —
        // hit-test и рендер всегда в одном кадре.
        RecalculatePositions();

        renderer_->Begin();

        target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

        const D2D1_ROUNDED_RECT rounded =
            D2D1::RoundedRect(
                D2D1::RectF(1.0f, 1.0f, W - 1.0f, H - 1.0f),
                config_.cornerRadius,
                config_.cornerRadius
            );


        ComPtr<ID2D1SolidColorBrush> panelBrush;

        target->CreateSolidColorBrush(
            D2D1::ColorF(
                config_.panelColorR / 255.0f,
                config_.panelColorG / 255.0f,
                config_.panelColorB / 255.0f,
                config_.transparencyLevel
            ),
            &panelBrush
        );

        if (panelBrush)
            target->FillRoundedRectangle(rounded, panelBrush.Get());


        if (config_.glassTint > 0.0f)
        {
            D2D1_GRADIENT_STOP stops[3];

            stops[0].position = 0.0f;
            stops[0].color = D2D1::ColorF(1.0f, 1.0f, 1.0f,
                config_.glassTint);
            stops[1].position = 0.45f;
            stops[1].color = D2D1::ColorF(1.0f, 1.0f, 1.0f,
                config_.glassTint * 0.35f);
            stops[2].position = 1.0f;
            stops[2].color = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f);

            ComPtr<ID2D1GradientStopCollection> stopCollection;

            target->CreateGradientStopCollection(
                stops, 3,
                D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP,
                &stopCollection
            );

            if (stopCollection)
            {
                ComPtr<ID2D1LinearGradientBrush> glassBrush;

                target->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(
                        D2D1::Point2F(0.0f, 0.0f),
                        D2D1::Point2F(0.0f, H)
                    ),
                    stopCollection.Get(),
                    &glassBrush
                );

                if (glassBrush)
                    target->FillRoundedRectangle(rounded, glassBrush.Get());
            }
        }


        ComPtr<ID2D1SolidColorBrush> borderBrush;

        target->CreateSolidColorBrush(
            D2D1::ColorF(
                config_.borderColorR / 255.0f,
                config_.borderColorG / 255.0f,
                config_.borderColorB / 255.0f,
                config_.borderAlpha
            ),
            &borderBrush
        );

        if (borderBrush)
            target->DrawRoundedRectangle(rounded, borderBrush.Get(), 1.0f);


        if (config_.highlightAlpha > 0.0f)
        {
            ComPtr<ID2D1SolidColorBrush> highlightBrush;

            target->CreateSolidColorBrush(
                D2D1::ColorF(1.0f, 1.0f, 1.0f, config_.highlightAlpha),
                &highlightBrush
            );

            if (highlightBrush)
            {
                const float inset = std::max(10.0f, config_.cornerRadius);

                target->DrawLine(
                    D2D1::Point2F(inset, 1.5f),
                    D2D1::Point2F(W - inset, 1.5f),
                    highlightBrush.Get(),
                    1.0f
                );
            }
        }


        for (const auto& icon : icons_)
            DrawIcon(target, icon);

        renderer_->End();
    }


    void DrawIcon(ID2D1RenderTarget* target, const RuntimeIcon& icon)
    {
        if (!icon.bitmap) return;

        const float size =
            static_cast<float>(IconSize()) * icon.scale;

        const D2D1_RECT_F destination =
            D2D1::RectF(
                icon.centerX - size * 0.5f,
                icon.centerY - size * 0.5f,
                icon.centerX + size * 0.5f,
                icon.centerY + size * 0.5f
            );

        target->DrawBitmap(
            icon.bitmap.Get(),
            destination,
            ClampFloat(icon.alpha, 0.0f, 1.0f)
        );
    }


private:

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;

    bool initialized_ = false;
    bool shutdown_ = false;

    bool mouseInside_ = false;
    bool leftButtonDown_ = false;
    bool internalDragging_ = false;

    int mouseDownX_ = 0;
    int mouseDownY_ = 0;
    int mouseDownIcon_ = -1;

    float hoverX_ = -100000.0f;
    float hoverY_ = -100000.0f;

    int windowWidth_ = 0;
    int windowHeight_ = 0;

    int gridColumns_ = 1;
    int gridRows_ = 1;

    std::chrono::steady_clock::time_point lastFrameTime_;

    DockConfig config_;
    ConfigManager configManager_;
    TaskbarManager taskbar_;

    Logger logger_;
    ConfigWatcher configWatcher_;

    std::unique_ptr<Renderer> renderer_;
    std::vector<RuntimeIcon> icons_;
};


// ============================================================
// GLOBAL / ENTRY
// ============================================================

static DockApplication* g_application = nullptr;


static BOOL WINAPI ConsoleHandler(DWORD signal)
{
    if (signal == CTRL_C_EVENT ||
        signal == CTRL_BREAK_EVENT ||
        signal == CTRL_CLOSE_EVENT ||
        signal == CTRL_LOGOFF_EVENT ||
        signal == CTRL_SHUTDOWN_EVENT)
    {
        if (g_application)
            g_application->Shutdown();

        return TRUE;
    }

    return FALSE;
}


int WINAPI wWinMain(
    HINSTANCE hInstance,
    HINSTANCE,
    PWSTR,
    int)
{
    if (!SetProcessDpiAwarenessContext(
        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
    {
        SetProcessDPIAware();
    }


    const HRESULT hr = CoInitializeEx(
        nullptr,
        COINIT_APARTMENTTHREADED
    );

    if (FAILED(hr)) return 1;


    SetConsoleCtrlHandler(ConsoleHandler, TRUE);


    DockApplication application;
    g_application = &application;

    if (!application.Initialize(hInstance))
    {
        application.Shutdown();
        g_application = nullptr;
        SetConsoleCtrlHandler(ConsoleHandler, FALSE);
        CoUninitialize();
        return 1;
    }


    const int result = application.Run();

    application.Shutdown();
    g_application = nullptr;
    SetConsoleCtrlHandler(ConsoleHandler, FALSE);
    CoUninitialize();

    return result;
}
