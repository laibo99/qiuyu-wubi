// 状态条离屏截图工具（仅本地验证用，不随安装包发布）
#include <windows.h>
#include <stdio.h>
#include "../core/qy_statusbar.h"

using namespace qy;

static void SaveBmp(HWND h, const wchar_t* path) {
    RECT rc;
    ::GetWindowRect(h, &rc);
    int w = rc.right - rc.left, ht = rc.bottom - rc.top;
    HDC sdc = ::GetDC(h);
    HDC mdc = ::CreateCompatibleDC(sdc);
    HBITMAP bmp = ::CreateCompatibleBitmap(sdc, w, ht);
    HBITMAP ob = (HBITMAP)::SelectObject(mdc, bmp);
    // 背景填成洋红以外的一种中性色，便于看出圆角
    RECT r{0, 0, w, ht};
    ::FillRect(mdc, &r, (HBRUSH)::GetStockObject(GRAY_BRUSH));
    ::PrintWindow(h, mdc, 0);

    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = ht;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biCompression = BI_RGB;
    int stride = ((w * 3 + 3) / 4) * 4;
    int imgSize = stride * ht;
    BYTE* bits = (BYTE*)::GlobalAlloc(GPTR, imgSize);
    ::GetDIBits(mdc, bmp, 0, ht, bits, (BITMAPINFO*)&ih, DIB_RGB_COLORS);

    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + imgSize;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") == 0 && f) {
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&ih, sizeof(ih), 1, f);
        fwrite(bits, imgSize, 1, f);
        fclose(f);
    }
    ::GlobalFree(bits);
    ::SelectObject(mdc, ob);
    ::DeleteObject(bmp);
    ::DeleteDC(mdc);
    ::ReleaseDC(h, sdc);
}

static void Pump(int ms) {
    DWORD end = ::GetTickCount() + ms;
    while (::GetTickCount() < end) {
        MSG m;
        while (::PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&m);
            ::DispatchMessageW(&m);
        }
        ::Sleep(10);
    }
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    StatusBar sb;
    if (!sb.Create(hInst)) {
        ::MessageBoxW(nullptr, L"状态条创建失败", L"test", MB_OK);
        return 1;
    }
    sb.SetPos(100, 100);
    StatusInfo si;
    si.chinese = true;
    si.mode = Mode::Wubi;
    sb.Update(si);
    sb.Show();
    Pump(300);
    SaveBmp(sb.hwnd(), L"sb_wubi.bmp");

    si.chinese = false;
    si.mode = Mode::Pinyin;
    sb.Update(si);
    ::InvalidateRect(sb.hwnd(), nullptr, TRUE);
    Pump(300);
    SaveBmp(sb.hwnd(), L"sb_pinyin.bmp");

    sb.Destroy();
    return 0;
}
