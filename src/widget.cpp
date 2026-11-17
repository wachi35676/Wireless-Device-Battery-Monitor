#include "widget.h"

#include <commctrl.h>
#include <uiautomation.h>
#include <algorithm>
#include <climits>
#include <cmath>
#include <memory>

namespace {

// One strip per taskbar: the primary one (Shell_TrayWnd, anchored left of TrayNotifyWnd) and one per
// secondary-monitor taskbar (Shell_SecondaryTrayWnd, anchored left of its clock, found through UI
// Automation because the Windows 11 secondary taskbar has no child windows for it).
struct Bar {
    HWND taskbar = nullptr, wnd = nullptr, rebar = nullptr;
    bool primary = false;
    bool xaml = false;     // Windows 11 XAML taskbar (Left placement is only offered there)
    bool hover = false;
    bool compact = false;  // tighter layout used when the full one does not fit
    bool fits = true;      // false: no room on this taskbar, the strip is hidden
    RECT lastAnchor{}, lastBar{}, lastWidgets{};
    int lastW = -1, lastDpi = 0, lastLeft = -1;
    SIZE size{};
    int fullW = 0, tightW = 0;  // widths of the two layouts, updated on every paint
    // Secondary taskbars: free span between the last task button and the clock (screen coords).
    int spanLeft = 0, spanRight = 0;
};

std::vector<std::unique_ptr<Bar>> g_bars;
HWND g_tip, g_notify;
HINSTANCE g_inst;
UINT g_clickMsg;
bool g_light;
bool g_background;  // opaque rounded plate behind the text, for transparent / restyled taskbars
bool g_others = true;  // also show on secondary-monitor taskbars
widget::Style g_style = widget::Style::Percent;
widget::Side g_side = widget::Side::Right;
std::vector<WidgetItem> g_items;
std::wstring g_tipText;

const wchar_t* kClass = L"WirelessBatteryWidget";

int Dpi(const Bar& b) {
    UINT d = b.taskbar ? GetDpiForWindow(b.taskbar) : 0;
    return d ? (int)d : 96;
}
int Px(const Bar& b, int v) { return MulDiv(v, Dpi(b), 96); }

Bar* FindBar(HWND wnd) {
    for (auto& b : g_bars)
        if (b->wnd == wnd) return b.get();
    return nullptr;
}

// Windows 11 puts the Widgets (weather) button right next to the tray when the taskbar is
// left-aligned (start-aligned on right-to-left Windows). It is XAML, so only UI Automation knows
// where it is. The element is looked up once per taskbar; its rectangle is re-read every few
// seconds because the weather text changes its width.
//
// UI Automation calls go to Explorer and wait for its UI thread, while our widget is a child window
// of the taskbar, so Explorer in turn waits for our UI thread whenever it lays out or sends input to
// the widget. Making those calls from our UI thread deadlocks both: Explorer is reported hung
// ("Cross-process", waiting on WirelessBattery.exe) and restarted, again and again. So the lookup
// runs on its own thread, which owns no windows, and the UI thread only reads the last rectangle.
SRWLOCK g_wbLock = SRWLOCK_INIT;
RECT g_wbRect{};             // last Widgets button rectangle (screen), empty when unknown
HWND g_wbRectBar;            // taskbar g_wbRect was found in (guarded by g_wbLock)
HWND g_wbTaskbar;            // taskbar to search, null while there is no widget (guarded by g_wbLock)
HANDLE g_wbWake;             // auto-reset: wakes the lookup thread early

DWORD WINAPI WidgetsButtonThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* uia = nullptr;
    IUIAutomationElement* btn = nullptr;
    HWND btnBar = nullptr;
    for (;;) {
        WaitForSingleObject(g_wbWake, btnBar ? 5000 : INFINITE);  // no widget (tray-icon mode): sleep until one
        AcquireSRWLockShared(&g_wbLock);
        HWND bar = g_wbTaskbar;
        ReleaseSRWLockShared(&g_wbLock);
        if (btn && bar != btnBar) { btn->Release(); btn = nullptr; }
        btnBar = bar;
        RECT r{};
        if (bar && IsWindow(bar)) {
            if (!uia)
                CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation), (void**)&uia);
            if (uia && !btn) {
                IUIAutomationElement* root = nullptr;
                IUIAutomationCondition* cond = nullptr;
                VARIANT id;
                VariantInit(&id);
                id.vt = VT_BSTR;
                id.bstrVal = SysAllocString(L"WidgetsButton");
                if (SUCCEEDED(uia->ElementFromHandle(bar, &root)) && root &&
                    SUCCEEDED(uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, id, &cond)) && cond)
                    root->FindFirst(TreeScope_Descendants, cond, &btn);
                VariantClear(&id);
                if (cond) cond->Release();
                if (root) root->Release();
            }
            if (btn && FAILED(btn->get_CurrentBoundingRectangle(&r))) {
                btn->Release();  // stale (Widgets turned off, Explorer restarted): look it up again next time
                btn = nullptr;
                r = {};
            }
        }
        AcquireSRWLockExclusive(&g_wbLock);
        if (bar && g_wbTaskbar == bar) {  // drop a result for a taskbar we no longer sit on
            g_wbRect = r;
            g_wbRectBar = bar;
        }
        ReleaseSRWLockExclusive(&g_wbLock);
    }
}

// Points the lookup thread at `taskbar` (null to idle it), starting the thread on first use.
// The last rectangle is kept for its taskbar, so switching from tray-icon back to taskbar mode on the
// same taskbar steps past the Widgets button straight away instead of after the next lookup.
void WatchWidgetsButton(HWND taskbar) {
    AcquireSRWLockExclusive(&g_wbLock);
    g_wbTaskbar = taskbar;
    ReleaseSRWLockExclusive(&g_wbLock);
    if (!g_wbWake) {
        g_wbWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE t = g_wbWake ? CreateThread(nullptr, 0, WidgetsButtonThread, nullptr, 0, nullptr) : nullptr;
        if (t) CloseHandle(t);
    }
    if (g_wbWake) SetEvent(g_wbWake);
}

// Never blocks: returns what the lookup thread last found on the current taskbar.
RECT WidgetsButtonRect() {
    AcquireSRWLockShared(&g_wbLock);
    RECT r = g_wbRectBar && g_wbRectBar == g_wbTaskbar ? g_wbRect : RECT{};
    ReleaseSRWLockShared(&g_wbLock);
    return r;
}

void ApplyTipTheme() {
    if (!g_tip) return;
    using SetWindowThemeFn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static auto setTheme = ux ? (SetWindowThemeFn)GetProcAddress(ux, "SetWindowTheme") : nullptr;
    if (setTheme) {
        setTheme(g_tip, g_light ? L"Explorer" : L"DarkMode_Explorer", nullptr);
        SendMessageW(g_tip, WM_THEMECHANGED, 0, 0);
    }
}

wchar_t Glyph(DeviceKind k, bool bluetooth) {
    switch (k) {
        case DeviceKind::Headset:  return 0xE7F6;  // Headphone
        case DeviceKind::Mouse:    return 0xE962;  // Mouse
        case DeviceKind::Keyboard: return 0xE765;  // KeyboardClassic
        case DeviceKind::Gamepad:  return 0xE7FC;  // Game
        case DeviceKind::Phone:    return 0xE8EA;  // CellPhone
        default:                   return bluetooth ? 0xE702 : 0xE772;  // Bluetooth / Devices
    }
}
constexpr wchar_t kBolt = 0xE945;            // LightningBolt
constexpr wchar_t kBatteryUnknown = 0xE996;  // shown when there is nothing to display

// Creates a font, falling back when the preferred face is not installed (GDI would silently substitute).
HFONT MakeFont(int px, int weight, const wchar_t* face, const wchar_t* fallback) {
    for (const wchar_t* f : { face, fallback }) {
        HFONT h = CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              ANTIALIASED_QUALITY, DEFAULT_PITCH, f);
        HDC dc = CreateCompatibleDC(nullptr);
        HGDIOBJ o = SelectObject(dc, h);
        wchar_t got[LF_FACESIZE] = {};
        GetTextFaceW(dc, LF_FACESIZE, got);
        SelectObject(dc, o);
        DeleteDC(dc);
        if (!_wcsicmp(got, f) || f == fallback) return h;
        DeleteObject(h);
    }
    return nullptr;
}

// Grayscale coverage mask of a string, cropped to its ink bounds.
struct Mask {
    int w = 0, h = 0;
    std::vector<BYTE> a;
};

Mask RenderMask(const std::wstring& s, HFONT font) {
    Mask m;
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ of = SelectObject(dc, font);
    SIZE sz;
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    int W = sz.cx + 8, H = sz.cy + 8;
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ ob = SelectObject(dc, bmp);
    memset(bits, 0, (size_t)W * H * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    TextOutW(dc, 4, 4, s.c_str(), (int)s.size());
    GdiFlush();
    auto* p = static_cast<DWORD*>(bits);
    int x0 = W, y0 = H, x1 = -1, y1 = -1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (p[y * W + x] & 0xFF) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
    if (x1 >= 0) {
        m.w = x1 - x0 + 1;
        m.h = y1 - y0 + 1;
        m.a.resize((size_t)m.w * m.h);
        for (int y = 0; y < m.h; y++)
            for (int x = 0; x < m.w; x++) m.a[y * m.w + x] = (BYTE)(p[(y0 + y) * W + x0 + x] & 0xFF);
    }
    SelectObject(dc, ob);
    SelectObject(dc, of);
    DeleteObject(bmp);
    DeleteDC(dc);
    return m;
}

struct Rgb { int r, g, b; };

struct Canvas {
    int w, h;
    std::vector<DWORD> px;  // premultiplied BGRA
    Canvas(int w_, int h_) : w(w_), h(h_), px((size_t)w_ * h_, 0) {}
    void Blend(int x, int y, Rgb c, int a) {  // source-over, a = 0..255
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) return;
        DWORD d = px[y * w + x];
        int inv = 255 - a;
        int da = d >> 24, dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
        int oa = a + da * inv / 255;
        int orr = c.r * a / 255 + dr * inv / 255;
        int og = c.g * a / 255 + dg * inv / 255;
        int ob = c.b * a / 255 + db * inv / 255;
        px[y * w + x] = ((DWORD)oa << 24) | ((DWORD)orr << 16) | ((DWORD)og << 8) | (DWORD)ob;
    }
    void Draw(const Mask& m, int x, int y, Rgb c, int alpha) {
        for (int j = 0; j < m.h; j++)
            for (int i = 0; i < m.w; i++) Blend(x + i, y + j, c, m.a[j * m.w + i] * alpha / 255);
    }
};

// One item on the strip: layers drawn centred on top of each other (text style: a single mask;
// ring style: track, arc and the device icon).
struct Layer { Mask m; Rgb color; int alpha; };
struct Piece { std::vector<Layer> layers; int w = 0; int gapAfter = 0; };

Piece One(Mask m, Rgb color, int alpha, int gapAfter) {
    Piece p;
    p.w = m.w;
    p.layers.push_back({ std::move(m), color, alpha });
    p.gapAfter = gapAfter;
    return p;
}

// Coverage masks for a ring of diameter d: the full circle (track) and the arc that starts at
// 12 o'clock and runs clockwise over `fraction` of it. 3x3 supersampled for smooth edges.
void RingMasks(int d, double stroke, double fraction, Mask& track, Mask& arc) {
    const double kTwoPi = 6.283185307179586;
    track.w = track.h = arc.w = arc.h = d;
    track.a.assign((size_t)d * d, 0);
    arc.a.assign((size_t)d * d, 0);
    double R = d / 2.0, inner = R - stroke;
    for (int y = 0; y < d; y++)
        for (int x = 0; x < d; x++) {
            int t = 0, a = 0;
            for (int sy = 0; sy < 3; sy++)
                for (int sx = 0; sx < 3; sx++) {
                    double px = x + (sx + 0.5) / 3 - R, py = y + (sy + 0.5) / 3 - R;
                    double r = std::sqrt(px * px + py * py);
                    if (r > R || r < inner) continue;
                    t++;
                    double ang = std::atan2(px, -py);
                    if (ang < 0) ang += kTwoPi;
                    if (ang <= fraction * kTwoPi) a++;
                }
            track.a[(size_t)y * d + x] = (BYTE)(t * 255 / 9);
            arc.a[(size_t)y * d + x] = (BYTE)(a * 255 / 9);
        }
}

// Compact: smaller gaps (and no "%" sign in the percentage style), for narrow taskbar spans.
std::vector<Piece> Layout(const Bar& b, bool compact, int& width) {
    Rgb fg = g_light ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 };
    Rgb red = g_light ? Rgb{ 196, 30, 40 } : Rgb{ 255, 90, 90 };
    Rgb charge = g_light ? Rgb{ 0, 120, 212 } : Rgb{ 96, 205, 255 };
    bool ring = g_style == widget::Style::Ring;
    HFONT icons = MakeFont(Px(b, ring ? 11 : 16), FW_NORMAL, L"Segoe Fluent Icons", L"Segoe MDL2 Assets");
    HFONT bolt = MakeFont(Px(b, 11), FW_NORMAL, L"Segoe Fluent Icons", L"Segoe MDL2 Assets");
    HFONT text = MakeFont(Px(b, 13), FW_NORMAL, L"Segoe UI Variable Text", L"Segoe UI");

    std::vector<Piece> out;
    if (g_items.empty()) out.push_back(One(RenderMask(std::wstring(1, kBatteryUnknown), icons), fg, 110, 0));
    for (size_t i = 0; i < g_items.size(); i++) {
        const auto& it = g_items[i];
        int a = it.dim ? 120 : 255;
        bool last = i + 1 == g_items.size();
        if (ring) {
            // Ring around the device icon: the arc is the charge, red when low, blue while charging.
            int d = Px(b, 28);  // even, so the ring and the icon share an exact centre
            d += d & 1;
            Mask track, arc;
            RingMasks(d, std::max(2.0, d / 13.0), it.percent >= 0 ? it.percent / 100.0 : 0.0, track, arc);
            Piece p;
            p.w = d;
            p.layers.push_back({ std::move(track), fg, a * (g_light ? 45 : 60) / 255 });
            p.layers.push_back({ std::move(arc), it.charging ? charge : it.low ? red : fg, a });
            p.layers.push_back({ RenderMask(std::wstring(1, Glyph(it.kind, it.bluetooth)), icons), fg, a });
            p.gapAfter = last ? 0 : Px(b, compact ? 5 : 8);
            out.push_back(std::move(p));
            continue;
        }
        Rgb tc = it.low ? red : fg;
        out.push_back(One(RenderMask(std::wstring(1, Glyph(it.kind, it.bluetooth)), icons), fg, a, Px(b, compact ? 3 : 5)));
        std::wstring t = !it.text.empty() ? it.text : it.percent >= 0 ? std::to_wstring(it.percent) + (compact ? L"" : L"%") : L"?";
        out.push_back(One(RenderMask(t, text), tc, a, it.charging ? Px(b, 2) : 0));
        if (it.charging) out.push_back(One(RenderMask(std::wstring(1, kBolt), bolt), charge, a, 0));
        out.back().gapAfter = last ? 0 : Px(b, compact ? 8 : 14);
    }
    DeleteObject(icons);
    DeleteObject(bolt);
    DeleteObject(text);

    width = Px(b, compact ? 4 : 8) * 2;
    for (auto& p : out) width += p.w + p.gapAfter;
    return out;
}

void Paint(Bar& b) {
    if (!b.wnd) return;
    RECT bar;
    GetClientRect(b.taskbar, &bar);
    int H = bar.bottom - bar.top;
    int W = 0;
    auto pieces = Layout(b, b.compact, W);
    int other = 0;
    Layout(b, !b.compact, other);  // the placement picks a layout by the room it has
    b.fullW = b.compact ? other : W;
    b.tightW = b.compact ? W : other;
    if (W <= 0 || H <= 0) return;
    Canvas cv(W, H);

    // Nearly transparent background so the whole strip receives mouse input,
    // plus a rounded hover highlight like the system tray icons have.
    for (auto& p : cv.px) p = 0x01000000;
    auto roundRect = [&](int inset, int r, Rgb c, int a) {
        for (int y = inset; y < H - inset; y++)
            for (int x = 0; x < W; x++) {
                int dx = x < r ? r - x : (x >= W - r ? x - (W - r - 1) : 0);
                int dy = y < inset + r ? inset + r - y : (y >= H - inset - r ? y - (H - inset - r - 1) : 0);
                if (dx * dx + dy * dy <= r * r) cv.Blend(x, y, c, a);
            }
    };
    // Optional plate in the taskbar's own colours: keeps the text readable when a taskbar theme
    // (e.g. Windhawk's Taskbar Styler) makes the bar transparent over the wallpaper.
    if (g_background) roundRect(Px(b, 4), Px(b, 6), g_light ? Rgb{ 243, 243, 243 } : Rgb{ 32, 32, 32 }, 240);
    if (b.hover) roundRect(Px(b, 4), Px(b, 4), g_light ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 }, g_light ? 20 : 26);

    int x = Px(b, b.compact ? 4 : 8);
    for (auto& p : pieces) {
        for (auto& l : p.layers) cv.Draw(l.m, x + (p.w - l.m.w) / 2, (H - l.m.h) / 2, l.color, l.alpha);
        x += p.w + p.gapAfter;
    }

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    memcpy(bits, cv.px.data(), (size_t)W * H * 4);
    HGDIOBJ ob = SelectObject(mem, bmp);
    b.size = { W, H };
    SIZE sz{ W, H };
    POINT src{ 0, 0 };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(b.wnd, screen, nullptr, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, ob);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    b.lastW = -1;  // force a re-position with the new size
}

void PaintAll() {
    for (auto& b : g_bars) Paint(*b);
}

// ---- UI Automation: where the clock and the task buttons are on secondary taskbars --------------
// Measured on a background thread: the widget is a child window of the taskbar being queried, so a
// UI Automation call from the UI thread deadlocks (Explorer's provider asks our window about itself
// while our thread waits for Explorer). The UI thread only reads the cached result.

struct Span {
    HWND taskbar = nullptr;
    bool hasClock = false, hasTask = false;  // the clock can be hidden; a taskbar can have no buttons
    bool hasStart = false, hasWidgets = false;
    int startLeft = 0;                       // left edge of the Start button
    int widgetsRight = 0;                    // right edge of the Widgets (weather) button
    int clockLeft = 0;                       // left edge of the clock (screen x, can be negative)
    int taskRight = 0;                       // right edge of the last task button
};

SRWLOCK g_spanLock = SRWLOCK_INIT;
std::vector<Span> g_spans;
HANDLE g_measureThread, g_measureStop;

IUIAutomationCondition* StringCondition(IUIAutomation* uia, PROPERTYID prop, const wchar_t* value) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(value);
    IUIAutomationCondition* c = nullptr;
    uia->CreatePropertyCondition(prop, v, &c);
    VariantClear(&v);
    return c;
}

struct Conditions { IUIAutomationCondition *clock = nullptr, *task = nullptr, *start = nullptr, *widgets = nullptr; };

// Bounds of the first element matching `cond` below `root`.
bool FindRect(IUIAutomationElement* root, IUIAutomationCondition* cond, RECT& r) {
    IUIAutomationElement* el = nullptr;
    if (!cond || FAILED(root->FindFirst(TreeScope_Descendants, cond, &el)) || !el) return false;
    bool ok = SUCCEEDED(el->get_CurrentBoundingRectangle(&r)) && r.right > r.left;
    el->Release();
    return ok;
}

// `clock` is only used on secondary taskbars (on the primary one the same id also matches tray buttons).
Span Measure(IUIAutomation* uia, const Conditions& cn, IUIAutomationCondition* clock, HWND taskbar) {
    IUIAutomationCondition* task = cn.task;
    Span sp;
    sp.taskbar = taskbar;
    RECT bar;
    GetWindowRect(taskbar, &bar);
    IUIAutomationElement* root = nullptr;
    if (FAILED(uia->ElementFromHandle(taskbar, &root)) || !root) return sp;
    IUIAutomationElement* el = nullptr;
    RECT r;
    if (FindRect(root, clock, r) && r.left > bar.left) {
        sp.clockLeft = r.left;
        sp.hasClock = true;
    }
    if (FindRect(root, cn.start, r)) {
        sp.startLeft = r.left;
        sp.hasStart = true;
    }
    if (FindRect(root, cn.widgets, r)) {
        sp.widgetsRight = r.right;
        sp.hasWidgets = true;
    }
    IUIAutomationElementArray* all = nullptr;
    if (task && SUCCEEDED(root->FindAll(TreeScope_Descendants, task, &all)) && all) {
        int n = 0;
        all->get_Length(&n);
        for (int i = 0; i < n; i++) {
            if (FAILED(all->GetElement(i, &el)) || !el) continue;
            if (SUCCEEDED(el->get_CurrentBoundingRectangle(&r)) && r.right > r.left && (!sp.hasClock || r.right <= sp.clockLeft)) {
                sp.taskRight = sp.hasTask ? std::max(sp.taskRight, (int)r.right) : (int)r.right;
                sp.hasTask = true;
            }
            el->Release();
        }
        all->Release();
    }
    root->Release();
    return sp;
}

DWORD WINAPI MeasureThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // CLSID_CUIAutomation / IID_IUIAutomation, spelled out so no extra import library is needed.
    static const GUID clsid = { 0xff48dba4, 0x60ef, 0x4201, { 0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e } };
    static const GUID iid = { 0x30cbe57d, 0xd9d0, 0x452a, { 0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee } };
    IUIAutomation* uia = nullptr;
    if (SUCCEEDED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, iid, (void**)&uia)) && uia) {
        Conditions cn;
        cn.clock = StringCondition(uia, UIA_AutomationIdPropertyId, L"SystemTrayIcon");
        cn.task = StringCondition(uia, UIA_ClassNamePropertyId, L"Taskbar.TaskListButtonAutomationPeer");
        cn.start = StringCondition(uia, UIA_AutomationIdPropertyId, L"StartButton");
        cn.widgets = StringCondition(uia, UIA_AutomationIdPropertyId, L"WidgetsButton");
        do {
            std::vector<Span> spans;
            if (HWND t = FindWindowW(L"Shell_TrayWnd", nullptr)) spans.push_back(Measure(uia, cn, nullptr, t));
            for (HWND t = FindWindowExW(nullptr, nullptr, L"Shell_SecondaryTrayWnd", nullptr); t;
                 t = FindWindowExW(nullptr, t, L"Shell_SecondaryTrayWnd", nullptr))
                spans.push_back(Measure(uia, cn, cn.clock, t));
            AcquireSRWLockExclusive(&g_spanLock);
            g_spans.swap(spans);
            ReleaseSRWLockExclusive(&g_spanLock);
        } while (WaitForSingleObject(g_measureStop, 2000) == WAIT_TIMEOUT);
        for (IUIAutomationCondition* x : { cn.clock, cn.task, cn.start, cn.widgets })
            if (x) x->Release();
        uia->Release();
    }
    CoUninitialize();
    return 0;
}

void StartMeasuring() {
    if (g_measureThread) return;
    g_measureStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_measureThread = CreateThread(nullptr, 0, MeasureThread, nullptr, 0, nullptr);
}

void StopMeasuring() {
    if (!g_measureThread) return;
    SetEvent(g_measureStop);
    WaitForSingleObject(g_measureThread, 3000);
    CloseHandle(g_measureThread);
    CloseHandle(g_measureStop);
    g_measureThread = g_measureStop = nullptr;
    AcquireSRWLockExclusive(&g_spanLock);
    g_spans.clear();
    ReleaseSRWLockExclusive(&g_spanLock);
}

// The measuring thread is only needed for secondary taskbars and for the Left placement.
bool NeedMeasuring() { return g_others || g_side == widget::Side::Left; }

// Latest measurement for a taskbar; false until the first one has come in.
bool GetSpan(HWND taskbar, Span& out) {
    bool found = false;
    AcquireSRWLockShared(&g_spanLock);
    for (const auto& sp : g_spans)
        if (sp.taskbar == taskbar) { out = sp; found = true; }
    ReleaseSRWLockShared(&g_spanLock);
    return found;
}

LRESULT CALLBACK WidgetProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Bar* b = FindBar(h);
    switch (msg) {
        case WM_MOUSEMOVE:
            if (b && !b->hover) {
                b->hover = true;
                TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 };
                TrackMouseEvent(&t);
                Paint(*b);
                widget::Reposition();
            }
            return 0;
        case WM_MOUSELEAVE:
            if (b) {
                b->hover = false;
                Paint(*b);
                widget::Reposition();
            }
            return 0;
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
            PostMessageW(g_notify, g_clickMsg, 0, 0);
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool Horizontal(HWND taskbar) {
    RECT tb;
    GetWindowRect(taskbar, &tb);
    return tb.right - tb.left >= tb.bottom - tb.top;
}

void AddTool(HWND wnd) {
    if (!g_tip) return;
    TTTOOLINFOW ti{ sizeof(ti) };
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = g_notify;
    ti.uId = (UINT_PTR)wnd;
    ti.lpszText = const_cast<wchar_t*>(g_tipText.empty() ? L"Wireless Battery" : g_tipText.c_str());
    SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

void RemoveTool(HWND wnd) {
    if (!g_tip) return;
    TTTOOLINFOW ti{ sizeof(ti) };
    ti.hwnd = g_notify;
    ti.uId = (UINT_PTR)wnd;
    SendMessageW(g_tip, TTM_DELTOOLW, 0, (LPARAM)&ti);
}

Bar* AddBar(HWND taskbar, bool primary) {
    auto b = std::make_unique<Bar>();
    b->taskbar = taskbar;
    b->primary = primary;
    b->xaml = FindWindowExW(taskbar, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr) != nullptr;
    // Windows 11's taskbar is a XAML island; on Windows 10 the task buttons live in ReBarWindow32
    // and have to be narrowed so they never slide under the widget.
    if (primary)
        b->rebar = FindWindowExW(taskbar, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr)
                       ? nullptr
                       : FindWindowExW(taskbar, nullptr, L"ReBarWindow32", nullptr);
    b->wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE, kClass, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 1, 1, taskbar,
                             nullptr, g_inst, nullptr);
    if (!b->wnd) return nullptr;
    AddTool(b->wnd);
    b->lastDpi = Dpi(*b);
    g_bars.push_back(std::move(b));
    Bar* p = g_bars.back().get();
    Paint(*p);
    return p;
}

void RemoveBar(size_t i) {
    Bar& b = *g_bars[i];
    if (b.rebar && IsWindow(b.rebar) && IsWindow(b.taskbar)) {
        // Let Explorer lay the task buttons out again over the full width.
        RECT r;
        GetClientRect(b.taskbar, &r);
        PostMessageW(b.taskbar, WM_SIZE, SIZE_RESTORED, MAKELPARAM(r.right, r.bottom));
    }
    if (b.wnd) {
        RemoveTool(b.wnd);
        DestroyWindow(b.wnd);
    }
    g_bars.erase(g_bars.begin() + i);
}

// Adds strips for new secondary taskbars and drops those whose taskbar is gone (all of them when
// showing on other taskbars is turned off).
void SyncSecondaries() {
    if (!g_others) {
        for (size_t i = g_bars.size(); i-- > 0;)
            if (!g_bars[i]->primary) RemoveBar(i);
        return;
    }
    for (size_t i = g_bars.size(); i-- > 0;)
        if (!g_bars[i]->primary && (!IsWindow(g_bars[i]->taskbar) || !Horizontal(g_bars[i]->taskbar))) RemoveBar(i);
    for (HWND t = FindWindowExW(nullptr, nullptr, L"Shell_SecondaryTrayWnd", nullptr); t;
         t = FindWindowExW(nullptr, t, L"Shell_SecondaryTrayWnd", nullptr)) {
        if (!Horizontal(t)) continue;
        bool known = false;
        for (auto& b : g_bars) known |= b->taskbar == t;
        if (!known && AddBar(t, false)) {
            Bar& b = *g_bars.back();
            ShowWindow(b.wnd, SW_SHOWNOACTIVATE);
        }
    }
}

// Picks the full or compact layout for `room` pixels; false when even the compact one does not fit.
bool Fit(Bar& b, int room) {
    bool compact = b.fullW > room;
    if (compact != b.compact) { b.compact = compact; Paint(b); }
    return (compact ? b.tightW : b.fullW) <= room;
}

void Hide(Bar& b) {
    if (IsWindowVisible(b.wnd)) ShowWindow(b.wnd, SW_HIDE);
}

// Places one strip. Returns false when its taskbar is gone.
bool PlaceBar(Bar& b) {
    if (!b.wnd || !IsWindow(b.wnd) || !IsWindow(b.taskbar)) return false;
    if (Dpi(b) != b.lastDpi) { b.lastDpi = Dpi(b); Paint(b); }
    RECT bar, anchor{}, wb{};
    GetWindowRect(b.taskbar, &bar);
    // Right-to-left Windows (Hebrew, Arabic) mirrors the taskbar: client x runs from the screen's right
    // edge, so a window's client-space left edge is its screen-space right edge.
    bool rtl = GetWindowLongW(b.taskbar, GWL_EXSTYLE) & WS_EX_LAYOUTRTL;
    Span sp;
    bool measured = GetSpan(b.taskbar, sp);
    int left = 0;  // client x of the strip

    // Left: from the taskbar's left edge (or the Widgets button) up to the Start button. Only on the
    // Windows 11 taskbar with centred icons; when there is no room it falls back to the right side.
    bool onLeft = false;
    if (g_side == widget::Side::Left && !rtl && b.xaml && measured && sp.hasStart) {
        int from = sp.hasWidgets ? sp.widgetsRight + Px(b, 4) : (int)bar.left + Px(b, 4);
        int to = sp.startLeft - Px(b, 4);
        if (to > from && Fit(b, to - from)) {
            onLeft = true;
            anchor = { from, bar.top, to, bar.bottom };
            POINT p{ from, bar.top };
            ScreenToClient(b.taskbar, &p);
            left = p.x;
        }
    }
    if (!onLeft && b.primary) {
        HWND tray = FindWindowExW(b.taskbar, nullptr, L"TrayNotifyWnd", nullptr);
        if (!tray) return true;
        GetWindowRect(tray, &anchor);
        if (b.compact) { b.compact = false; Paint(b); }  // the primary tray side always has room
        int edge = rtl ? anchor.right : anchor.left;  // screen x we sit next to
        // Windows 10: News and Interests / Weather sits in ShellFeedsTaskbarViewWindow immediately left of the tray.
        HWND feeds = FindWindowExW(b.taskbar, nullptr, L"ShellFeedsTaskbarViewWindow", nullptr);
        if (!rtl && feeds && IsWindowVisible(feeds)) {
            RECT fr;
            GetWindowRect(feeds, &fr);
            if (fr.left < edge && fr.right > fr.left) edge = fr.left;
        }
        // Step past the Widgets button when it sits where we would go.
        wb = WidgetsButtonRect();
        if (wb.right > wb.left && wb.top < bar.bottom && wb.bottom > bar.top &&
            (rtl ? wb.left < edge + b.size.cx + Px(b, 2) && wb.right > edge : wb.right > edge - b.size.cx - Px(b, 2) && wb.left < edge))
            edge = rtl ? wb.right : wb.left;
        POINT p{ edge, bar.top };
        ScreenToClient(b.taskbar, &p);
        left = p.x - b.size.cx - Px(b, 2);

        // Optional user override in DPI-independent pixels (e.g. for custom padding).
        DWORD customOffset = 0, sz = sizeof(customOffset);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\WirelessBattery", L"TaskbarOffset",
                         RRF_RT_REG_DWORD, nullptr, &customOffset, &sz) == ERROR_SUCCESS) {
            left -= Px(b, (int)customOffset);
        }
    } else if (!onLeft) {
        if (!measured) { Hide(b); return true; }  // not measured yet
        b.spanLeft = sp.hasTask ? std::max((int)bar.left, sp.taskRight) : (int)bar.left;
        b.spanRight = sp.hasClock ? sp.clockLeft : bar.right - Px(b, 8);
        if (!Fit(b, b.spanRight - b.spanLeft - Px(b, 4))) { Hide(b); return true; }
        anchor = { b.spanLeft, bar.top, b.spanRight, bar.bottom };
        POINT p{ b.spanRight - Px(b, 2), bar.top };
        ScreenToClient(b.taskbar, &p);
        left = p.x - b.size.cx;
    }

    // Windows 10: Explorer re-expands the task buttons on its own layout passes; narrow them again.
    if (!onLeft && b.rebar && IsWindow(b.rebar)) {
        RECT rr;
        GetWindowRect(b.rebar, &rr);
        POINT tl{ rtl ? rr.right : rr.left, rr.top };
        ScreenToClient(b.taskbar, &tl);
        if (tl.x + (rr.right - rr.left) > left && left > tl.x)
            MoveWindow(b.rebar, tl.x, tl.y, left - tl.x, rr.bottom - rr.top, TRUE);
    }

    // Keep us above the taskbar's own content (our sibling windows).
    bool covered = GetWindow(b.wnd, GW_HWNDPREV) != nullptr;
    if (!covered && IsWindowVisible(b.wnd) && EqualRect(&anchor, &b.lastAnchor) && EqualRect(&bar, &b.lastBar) &&
        EqualRect(&wb, &b.lastWidgets) && b.lastW == b.size.cx && b.lastLeft == left)
        return true;
    b.lastAnchor = anchor;
    b.lastWidgets = wb;
    b.lastLeft = left;
    b.lastBar = bar;
    b.lastW = b.size.cx;
    SetWindowPos(b.wnd, HWND_TOP, left, 0, b.size.cx, b.size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
}

}  // namespace

namespace widget {

bool Create(HINSTANCE inst, HWND notifyWnd, UINT clickMsg, bool lightTheme) {
    Destroy();
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!taskbar || !FindWindowExW(taskbar, nullptr, L"TrayNotifyWnd", nullptr)) return false;
    if (!Horizontal(taskbar)) return false;  // vertical taskbar: use tray icons
    g_inst = inst;
    g_notify = notifyWnd;
    g_clickMsg = clickMsg;
    g_light = lightTheme;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WidgetProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        registered = RegisterClassExW(&wc) != 0;
    }

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);
    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0,
                            notifyWnd, nullptr, inst, nullptr);
    if (g_tip) {
        ApplyTipTheme();
        SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, 600);
    }
    WatchWidgetsButton(taskbar);

    if (NeedMeasuring()) StartMeasuring();
    Bar* primary = AddBar(taskbar, true);
    if (!primary) {
        Destroy();
        return false;
    }
    PlaceBar(*primary);
    ShowWindow(primary->wnd, SW_SHOWNOACTIVATE);
    SyncSecondaries();
    for (auto& b : g_bars)
        if (!b->primary) PlaceBar(*b);
    return true;
}

void Destroy() {
    StopMeasuring();
    while (!g_bars.empty()) RemoveBar(g_bars.size() - 1);
    if (g_wbWake) WatchWidgetsButton(nullptr);
    if (g_tip) { DestroyWindow(g_tip); g_tip = nullptr; }
}

bool Alive() {
    for (auto& b : g_bars)
        if (b->primary) return b->wnd && IsWindow(b->wnd) && IsWindow(b->taskbar);
    return false;
}

void SetItems(const std::vector<WidgetItem>& items, const std::wstring& tooltip) {
    g_items = items;
    if (tooltip != g_tipText && g_tip) {
        g_tipText = tooltip;
        for (auto& b : g_bars) {
            TTTOOLINFOW ti{ sizeof(ti) };
            ti.hwnd = g_notify;
            ti.uId = (UINT_PTR)b->wnd;
            ti.lpszText = const_cast<wchar_t*>(g_tipText.c_str());
            SendMessageW(g_tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
        }
    }
    PaintAll();
    Reposition();
}

void SetOtherTaskbars(bool on) {
    g_others = on;
    if (!NeedMeasuring()) StopMeasuring();
    else if (!g_bars.empty()) StartMeasuring();
    Reposition();
}

void SetStyle(Style style) {
    g_style = style;
    PaintAll();
    Reposition();
}

void SetSide(Side side) {
    g_side = side;
    if (!NeedMeasuring()) StopMeasuring();
    else if (!g_bars.empty()) StartMeasuring();
    for (auto& b : g_bars) b->lastW = -1;  // re-place every strip
    Reposition();
}

bool HasOtherTaskbars() {
    for (HWND t = FindWindowExW(nullptr, nullptr, L"Shell_SecondaryTrayWnd", nullptr); t;
         t = FindWindowExW(nullptr, t, L"Shell_SecondaryTrayWnd", nullptr))
        if (Horizontal(t)) return true;
    return false;
}

void SetLightTheme(bool light) {
    g_light = light;
    ApplyTipTheme();
    PaintAll();
}

void SetBackground(bool on) {
    g_background = on;
    PaintAll();
}

bool Reposition() {
    if (!Alive()) return false;
    SyncSecondaries();
    for (auto& b : g_bars) PlaceBar(*b);
    return true;
}

}  // namespace widget
