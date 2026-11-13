// 秋雨五笔输入法 - 候选窗口实现
#include "qy_candui.h"
#include <algorithm>

namespace qy {

static const wchar_t* kClassName = L"QiuYuWubiCandWnd";

bool CandWindow::IsDarkMode() const {
    HKEY k;
    DWORD v = 0, sz = sizeof(v);
    if (::RegOpenKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                        KEY_READ, &k) == ERROR_SUCCESS) {
        ::RegQueryValueExW(k, L"AppsUseLightTheme", nullptr, nullptr, (LPBYTE)&v, &sz);
        ::RegCloseKey(k);
    }
    return v == 0;
}

bool CandWindow::Create(HINSTANCE hInst) {
    if (hwnd_) return true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    ::RegisterClassExW(&wc);
    hwnd_ = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, kClassName, L"",
                              WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hInst, this);
    return hwnd_ != nullptr;
}

void CandWindow::Destroy() {
    if (font_) { ::DeleteObject(font_); font_ = nullptr; }
    if (fontSmall_) { ::DeleteObject(fontSmall_); fontSmall_ = nullptr; }
    if (hwnd_) {
        ::KillTimer(hwnd_, 1);
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void CandWindow::Hide() {
    if (hwnd_ && visible_) {
        ::ShowWindow(hwnd_, SW_HIDE);
        visible_ = false;
    }
}

void CandWindow::CheckForeground() {
    if (!hwnd_ || !visible_) return;
    HWND fg = ::GetForegroundWindow();
    DWORD pid = 0;
    if (fg) ::GetWindowThreadProcessId(fg, &pid);
    // 前台窗口属于别的进程 → 本实例的候选窗立即收起
    if (pid != ::GetCurrentProcessId()) Hide();
}

LRESULT CALLBACK CandWindow::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    CandWindow* self = (CandWindow*)::GetWindowLongPtrW(h, GWLP_USERDATA);
    if (m == WM_CREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)l;
        self = (CandWindow*)cs->lpCreateParams;
        ::SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        if (self) self->hwnd_ = h;
        ::SetTimer(h, 1, 400, nullptr);  // 监视前台窗口归属，切到别的程序就自动收起
        return 0;
    }
    if (!self) return ::DefWindowProcW(h, m, w, l);
    switch (m) {
        case WM_ERASEBKGND:
            return 1;
        case WM_TIMER:
            if (w == 1) {
                self->CheckForeground();
                return 0;
            }
            break;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            ::BeginPaint(h, &ps);
            self->OnPaint();
            ::EndPaint(h, &ps);
            return 0;
        }
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_LBUTTONDOWN: {
            POINT pt{(LONG)(short)LOWORD(l), (LONG)(short)HIWORD(l)};
            ::ClientToScreen(h, &pt);
            int idx = self->HitTest(pt);
            if (idx >= 0 && self->onClick_) self->onClick_(idx);
            return 0;
        }
        default:
            break;
    }
    return ::DefWindowProcW(h, m, w, l);
}

int CandWindow::HitTest(POINT screenPt) const {
    if (!visible_) return -1;
    POINT c = screenPt;
    ::ScreenToClient(hwnd_, &c);
    for (size_t i = 0; i < itemRects_.size(); ++i) {
        if (::PtInRect(&itemRects_[i], c)) return (int)i;
    }
    return -1;
}

void CandWindow::Show(const CandShow& p) {
    if (!hwnd_ || !p.items || p.items->empty()) { Hide(); return; }
    data_ = p;
    items_.clear();
    size_t start = p.page * p.pageSize;
    size_t end = std::min(start + p.pageSize, p.items->size());
    for (size_t i = start; i < end; ++i) items_.push_back((*p.items)[i]);
    if (items_.empty()) { Hide(); return; }
    pageCount_ = (p.items->size() + p.pageSize - 1) / p.pageSize;
    if (pageCount_ == 0) pageCount_ = 1;
    fontSize_ = p.fontSize;

    dark_ = IsDarkMode();
    if (font_) ::DeleteObject(font_);
    if (fontSmall_) ::DeleteObject(fontSmall_);
    font_ = ::CreateFontW(-fontSize_, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
    fontSmall_ = ::CreateFontW(-std::max(11, fontSize_ * 3 / 5), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                               FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");

    HDC screen = ::GetDC(nullptr);
    HDC dc = ::CreateCompatibleDC(screen);

    // 计算尺寸
    int padX = std::max(6, fontSize_ / 3);
    int padY = std::max(4, fontSize_ / 5);
    int rowH = (int)(fontSize_ * 1.7);
    int numW = (int)(fontSize_ * 0.85);
    int codeH = (int)(fontSize_ * 0.75);

    struct Metric { int w; int textW; int codeW; };
    std::vector<Metric> metrics;
    ::SelectObject(dc, font_);
    int maxTextW = 0, maxCodeW = 0;
    for (size_t i = 0; i < items_.size(); ++i) {
        SIZE sz{0, 0};
        ::GetTextExtentPoint32W(dc, items_[i].text.c_str(), (int)items_[i].text.size(), &sz);
        int tw = sz.cx;
        int cw = 0;
        if (data_.showCode && !items_[i].code.empty()) {
            ::SelectObject(dc, fontSmall_);
            std::wstring code = Utf8ToW(items_[i].code);
            SIZE cs{0, 0};
            ::GetTextExtentPoint32W(dc, code.c_str(), (int)code.size(), &cs);
            cw = cs.cx + 4;
            ::SelectObject(dc, font_);
        }
        Metric m{padX + numW + 4 + tw + cw + padX, tw, cw};
        metrics.push_back(m);
        maxTextW = std::max(maxTextW, tw);
        maxCodeW = std::max(maxCodeW, cw);
    }
    // 翻页提示
    int tipW = 0;
    if (data_.showTip && pageCount_ > 1) {
        ::SelectObject(dc, fontSmall_);
        wchar_t tip[32];
        wsprintfW(tip, L"%u/%u", (unsigned)(data_.page + 1), (unsigned)pageCount_);
        SIZE ts{0, 0};
        ::GetTextExtentPoint32W(dc, tip, (int)wcslen(tip), &ts);
        tipW = ts.cx + padX * 2;
        ::SelectObject(dc, font_);
    }

    int w, h;
    itemRects_.clear();
    if (data_.horizontal) {
        w = tipW;
        for (auto& m : metrics) w += m.w;
        h = rowH + padY * 2;
        int x = 0;
        for (size_t i = 0; i < metrics.size(); ++i) {
            RECT r{x, 0, x + metrics[i].w, h};
            itemRects_.push_back(r);
            x += metrics[i].w;
        }
    } else {
        int itemW = padX + numW + 4 + maxTextW + maxCodeW + padX;
        w = itemW + tipW;
        h = (int)items_.size() * rowH + padY * 2;
        int y = padY;
        for (size_t i = 0; i < metrics.size(); ++i) {
            RECT r{0, y, itemW, y + rowH};
            itemRects_.push_back(r);
            y += rowH;
        }
    }
    ::DeleteDC(dc);
    ::ReleaseDC(nullptr, screen);

    // 定位（避免超出工作区）
    int x = p.pt.x, y = p.pt.y;
    HMONITOR mon = ::MonitorFromPoint(p.pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(MONITORINFO)};
    ::GetMonitorInfoW(mon, &mi);
    if (x + w > mi.rcWork.right) x = mi.rcWork.right - w;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y + h > mi.rcWork.bottom) y = p.pt.y - h - (int)(fontSize_ * 1.9);  // 翻到光标上方
    if (y < mi.rcWork.top) y = mi.rcWork.top;

    ::SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    HRGN rgn = ::CreateRoundRectRgn(0, 0, w + 1, h + 1, 10, 10);
    ::SetWindowRgn(hwnd_, rgn, TRUE);
    visible_ = true;
    ::InvalidateRect(hwnd_, nullptr, TRUE);
    ::UpdateWindow(hwnd_);
}

void CandWindow::OnPaint() {
    HDC dc = ::GetDC(hwnd_);
    RECT rc;
    ::GetClientRect(hwnd_, &rc);
    HDC mem = ::CreateCompatibleDC(dc);
    HBITMAP bmp = ::CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HBITMAP old = (HBITMAP)::SelectObject(mem, bmp);

    COLORREF bg = dark_ ? RGB(42, 44, 48) : RGB(252, 252, 253);
    COLORREF border = dark_ ? RGB(72, 76, 84) : RGB(206, 210, 218);
    COLORREF text = dark_ ? RGB(236, 238, 242) : RGB(28, 30, 34);
    COLORREF selBg = dark_ ? RGB(38, 70, 112) : RGB(214, 232, 255);
    COLORREF accent = dark_ ? RGB(122, 176, 240) : RGB(24, 96, 180);
    COLORREF codeCol = dark_ ? RGB(140, 146, 156) : RGB(150, 156, 166);

    HBRUSH b = ::CreateSolidBrush(bg);
    ::FillRect(mem, &rc, b);
    ::DeleteObject(b);
    HPEN pen = ::CreatePen(PS_SOLID, 1, border);
    HPEN oldPen = (HPEN)::SelectObject(mem, pen);
    HBRUSH oldBrush = (HBRUSH)::SelectObject(mem, ::GetStockObject(NULL_BRUSH));
    ::Rectangle(mem, 0, 0, rc.right, rc.bottom);
    ::SelectObject(mem, oldBrush);
    ::SelectObject(mem, oldPen);
    ::DeleteObject(pen);

    ::SetBkMode(mem, TRANSPARENT);

    int padX = std::max(6, fontSize_ / 3);
    int numW = (int)(fontSize_ * 0.85);
    for (size_t i = 0; i < itemRects_.size() && i < items_.size(); ++i) {
        RECT r = itemRects_[i];
        if ((int)i == data_.sel) {
            HBRUSH sb = ::CreateSolidBrush(selBg);
            RECT rr = r;
            ::InflateRect(&rr, 0, -2);
            ::FillRect(mem, &rr, sb);
            ::DeleteObject(sb);
        }
        int x = r.left + padX;
        int cy = (r.top + r.bottom) / 2;
        // 序号
        ::SelectObject(mem, fontSmall_);
        ::SetTextColor(mem, (int)i == data_.sel ? accent : codeCol);
        wchar_t num[4];
        wsprintfW(num, L"%u", (unsigned)(i + 1));
        SIZE ns{0, 0};
        ::GetTextExtentPoint32W(mem, num, (int)wcslen(num), &ns);
        ::TextOutW(mem, x, cy - ns.cy / 2, num, (int)wcslen(num));
        x += numW + 4;
        // 候选字
        ::SelectObject(mem, font_);
        ::SetTextColor(mem, text);
        SIZE ts{0, 0};
        ::GetTextExtentPoint32W(mem, items_[i].text.c_str(), (int)items_[i].text.size(), &ts);
        ::TextOutW(mem, x, cy - ts.cy / 2, items_[i].text.c_str(), (int)items_[i].text.size());
        x += ts.cx + 4;
        // 编码
        if (data_.showCode && !items_[i].code.empty()) {
            ::SelectObject(mem, fontSmall_);
            ::SetTextColor(mem, codeCol);
            std::wstring code = Utf8ToW(items_[i].code);
            SIZE cs{0, 0};
            ::GetTextExtentPoint32W(mem, code.c_str(), (int)code.size(), &cs);
            ::TextOutW(mem, x, cy - cs.cy / 2 + 1, code.c_str(), (int)code.size());
        }
    }
    // 翻页提示
    if (data_.showTip && pageCount_ > 1) {
        ::SelectObject(mem, fontSmall_);
        ::SetTextColor(mem, codeCol);
        wchar_t tip[32];
        wsprintfW(tip, L"%u/%u", (unsigned)(data_.page + 1), (unsigned)pageCount_);
        SIZE ts{0, 0};
        ::GetTextExtentPoint32W(mem, tip, (int)wcslen(tip), &ts);
        ::TextOutW(mem, rc.right - ts.cx - padX, rc.bottom - ts.cy - 3, tip, (int)wcslen(tip));
    }

    ::BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    ::SelectObject(mem, old);
    ::DeleteObject(bmp);
    ::DeleteDC(mem);
    ::ReleaseDC(hwnd_, dc);
}

}  // namespace qy
