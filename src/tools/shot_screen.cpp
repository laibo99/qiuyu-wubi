// 截取屏幕指定区域到 BMP，用于目视验证窗口渲染
#include <windows.h>
#include <stdio.h>
#include <shellapi.h>

int main() {
    int argc = 0;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    int x = 260, y = 140, w = 890, h = 520;
    const wchar_t* out = L"build/screen.bmp";
    if (argc >= 5) {
        x = _wtoi(argv[1]);
        y = _wtoi(argv[2]);
        w = _wtoi(argv[3]);
        h = _wtoi(argv[4]);
    }
    if (argc >= 6) out = argv[5];

    HDC screen = ::GetDC(nullptr);
    HDC mem = ::CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = ::CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = ::SelectObject(mem, bmp);
    ::BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT);

    int stride = w * 4;
    DWORD imgSize = (DWORD)stride * h;
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + imgSize;
    BITMAPINFOHEADER ih = bi.bmiHeader;
    ih.biSizeImage = imgSize;

    FILE* f = nullptr;
    if (_wfopen_s(&f, out, L"wb") == 0 && f) {
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&ih, sizeof(ih), 1, f);
        fwrite(bits, 1, imgSize, f);
        fclose(f);
        printf("saved %ls\n", out);
    } else {
        printf("write failed\n");
    }
    ::SelectObject(mem, old);
    ::DeleteObject(bmp);
    ::DeleteDC(mem);
    ::ReleaseDC(nullptr, screen);
    if (argv) ::LocalFree(argv);
    return 0;
}
