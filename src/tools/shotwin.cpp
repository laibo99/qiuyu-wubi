// 截取指定类名窗口保存 BMP
#include <windows.h>
#include <stdio.h>
static bool Save(HDC src, int x, int y, int w, int h, const wchar_t* path) {
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC mem = CreateCompatibleDC(src);
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    // CAPTUREBLT：把分层窗口（UpdateLayeredWindow）也合成进来
    BitBlt(mem, 0, 0, w, h, src, x, y, SRCCOPY | CAPTUREBLT);
    int stride = w * 4; DWORD imgSize = (DWORD)stride * h;
    BITMAPFILEHEADER fh{}; fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER); fh.bfSize = fh.bfOffBits + imgSize;
    BITMAPINFOHEADER ih = bi.bmiHeader; ih.biHeight = -h; ih.biSizeImage = imgSize;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) return false;
    fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f); fwrite(bits, 1, imgSize, f); fclose(f);
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
    return true;
}
int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: shotwin <cls> [out]\n"); return 1; }
    wchar_t cls[128]; MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, cls, 128);
    wchar_t out[128] = L"build/shot.bmp";
    if (argc >= 3) MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, out, 128);
    HWND h = FindWindowW(cls, nullptr);
    if (!h) { printf("window not found\n"); return 2; }
    // 置前再截，避免被其它窗口遮挡
    SetForegroundWindow(h);
    Sleep(400);
    RECT r; GetWindowRect(h, &r);
    HDC dc = GetDC(nullptr);
    bool ok = Save(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, out);
    ReleaseDC(nullptr, dc);
    printf("rect=%ld,%ld %ldx%ld save=%d\n", r.left, r.top, r.right - r.left, r.bottom - r.top, (int)ok);
    return ok ? 0 : 3;
}
