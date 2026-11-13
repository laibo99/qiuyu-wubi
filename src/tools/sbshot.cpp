// 状态条渲染自检：真实创建状态条窗口，截取屏幕区域放大保存为 BMP
// 用法：sbshot.exe [accent_hex] [card_hex] [out_prefix]
//   例：sbshot.exe 3A7BD5 141E2E build/custom
// 输出：<prefix>_1x.bmp（实际大小）、<prefix>_4x.bmp（4 倍放大）
// 用来肉眼确认“调色盘选任意颜色”之后的可读性：深底浅字、浅胶囊深字等。
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include "../core/qy_statusbar.h"

using namespace qy;

static bool SaveRegion(const wchar_t* path, int x, int y, int w, int h, int scale) {
    HDC screen = ::GetDC(nullptr);
    HDC mem = ::CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w * scale;
    bi.bmiHeader.biHeight = -h * scale;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = ::CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = ::SelectObject(mem, bmp);
    if (scale == 1) {
        ::BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT);
    } else {
        ::SetStretchBltMode(mem, HALFTONE);
        ::SetBrushOrgEx(mem, 0, 0, nullptr);
        ::StretchBlt(mem, 0, 0, w * scale, h * scale, screen, x, y, w, h, SRCCOPY | CAPTUREBLT);
    }

    int stride = w * scale * 4;
    DWORD imgSize = (DWORD)stride * h * scale;
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + imgSize;
    BITMAPINFOHEADER ih = bi.bmiHeader;
    ih.biHeight = -h * scale;
    ih.biSizeImage = imgSize;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) return false;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(bits, 1, imgSize, f);
    fclose(f);

    ::SelectObject(mem, old);
    ::DeleteObject(bmp);
    ::DeleteDC(mem);
    ::ReleaseDC(nullptr, screen);
    return true;
}

static void Pump(int ms) {
    DWORD end = ::GetTickCount() + (DWORD)ms;
    MSG msg;
    while (::GetTickCount() < end) {
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        ::Sleep(10);
    }
}

int main() {
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    // 参数按网页习惯 #RRGGBB 解析（COLORREF 内存里是 BGR，别搞反）
    auto parseHex = [](const wchar_t* s) -> COLORREF {
        wchar_t* e = nullptr;
        unsigned long v = wcstoul(s, &e, 16);
        if (e - s != 6) return 0;
        return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
    };
    COLORREF accent = 0, card = 0;
    const wchar_t* prefix = L"build/sb_custom";
    if (argc > 1 && argv[1][0]) accent = parseHex(argv[1]);
    if (argc > 2 && argv[2][0]) card = parseHex(argv[2]);
    if (argc > 3 && argv[3][0]) prefix = argv[3];

    HINSTANCE inst = ::GetModuleHandleW(nullptr);
    StatusBar sb;
    if (!sb.Create(inst)) {
        printf("Create failed\n");
        return 1;
    }
    // 自检程序没有别的窗口：让前台探针返回自己，否则定时器会把条收起来
    HWND self = sb.hwnd();
    sb.SetForegroundProbe([self]() { return self; });

    StatusSkin sk;
    sk.theme = 1;  // 浅色主题：用来验证“浅色主题 + 深色自定义底”
    sk.translucent = 0;
    sk.accent = accent;
    sk.card = card;
    sb.SetSkin(sk);

    StatusInfo si;
    si.chinese = true;
    si.mode = Mode::Mixed;
    si.fullWidth = false;
    si.symbols = true;
    sb.SetEnabled(true);
    sb.Update(si);
    sb.SetPos(160, 160);
    sb.Show();
    ::RedrawWindow(sb.hwnd(), nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    Pump(400);

    wchar_t p1[MAX_PATH], p2[MAX_PATH];
    ::wsprintfW(p1, L"%s_1x.bmp", prefix);
    ::wsprintfW(p2, L"%s_4x.bmp", prefix);
    ::Sleep(80);
    bool ok = SaveRegion(p2, 160, 160, 172, 36, 4);
    SaveRegion(p1, 160, 160, 172, 36, 1);

    printf("hwnd=%p accent=%06lX card=%06lX save=%d\n", (void*)sb.hwnd(), (unsigned long)accent,
           (unsigned long)card, (int)ok);
    sb.Hide();
    sb.Destroy();
    ::LocalFree(argv);
    return ok ? 0 : 2;
}
