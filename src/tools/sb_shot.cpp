// 状态条真实屏幕截图：把窗口所在屏幕区域抓成 BMP
#include <cstdio>
#include <windows.h>
#include "../core/qy_statusbar.h"

static void SaveBmp(const wchar_t* path, HDC src, int x, int y, int w, int h) {
    HDC mem = ::CreateCompatibleDC(src);
    HBITMAP bmp = ::CreateCompatibleBitmap(src, w, h);
    HBITMAP ob = (HBITMAP)::SelectObject(mem, bmp);
    ::BitBlt(mem, 0, 0, w, h, src, x, y, SRCCOPY);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    int stride = w * 4;
    unsigned char* buf = new unsigned char[stride * h];
    ::GetDIBits(mem, bmp, 0, h, buf, &bi, DIB_RGB_COLORS);

    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + stride * h;
    FILE* f = _wfopen(path, L"wb");
    if (f) {
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
        fwrite(buf, 1, stride * h, f);
        fclose(f);
    }
    delete[] buf;
    ::SelectObject(mem, ob);
    ::DeleteObject(bmp);
    ::DeleteDC(mem);
}

static const wchar_t* g_name = nullptr;
static const wchar_t* g_names[5] = {L"wubi", L"pinyin", L"full", L"numpad_off", L"english"};

int main(int argc, char** argv) {
    if (argc > 1) {
        static wchar_t buf[64];
        ::MultiByteToWideChar(CP_ACP, 0, argv[1], -1, buf, 64);
        g_name = buf;
    }
    HINSTANCE h = ::GetModuleHandleW(nullptr);
    qy::StatusBar sb;
    if (!sb.Create(h)) return 2;

    // 参数：p=Pinyin f=全角 n=小键盘关 e=英文
    bool pin = false, fw = false, np = true, en = false;
    for (int i = 1; i < argc; ++i) {
        for (const char* c = argv[i]; *c; ++c) {
            if (*c == 'p') pin = true;
            if (*c == 'f') fw = true;
            if (*c == 'n') np = false;
            if (*c == 'e') en = true;
        }
    }
    qy::StatusInfo si;
    si.chinese = !en;
    si.mode = pin ? qy::Mode::Pinyin : qy::Mode::Wubi;
    si.fullWidth = fw;
    si.numpad = np;
    sb.Update(si);
    sb.SetPos(120, 120);
    sb.Show();
    ::UpdateWindow(sb.hwnd());
    ::Sleep(400);

    auto pw = [&](int n) {
        if (!g_name) return false;
        return _wcsicmp(g_name, g_names[n - 1]) == 0;
    };
    HDC screen = ::GetDC(nullptr);
    const wchar_t* out = L"build/shot_default.bmp";
    if (pw(1)) out = L"build/shot_wubi.bmp";
    if (pw(2)) out = L"build/shot_pinyin.bmp";
    if (pw(3)) out = L"build/shot_full.bmp";
    if (pw(4)) out = L"build/shot_numpad_off.bmp";
    if (pw(5)) out = L"build/shot_english.bmp";
    SaveBmp(out, screen, 118, 118, 148, 32);
    ::ReleaseDC(nullptr, screen);
    sb.Hide();
    sb.Destroy();
    return 0;
}
