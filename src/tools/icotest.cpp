// 校验 PE 内嵌图标资源：查找 RT_GROUP_ICON(101)，并把图标渲染成 BMP 便于目视检查
#include <windows.h>
#include <stdio.h>
#include <shellapi.h>

static bool SaveBmp(const wchar_t* path, HICON icon, int size) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screen = ::GetDC(nullptr);
    HDC dc = ::CreateCompatibleDC(screen);
    HBITMAP bmp = ::CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = ::SelectObject(dc, bmp);

    // 背景涂成深灰，便于看清图标边缘
    RECT r{0, 0, size, size};
    HBRUSH bg = ::CreateSolidBrush(RGB(40, 40, 40));
    ::FillRect(dc, &r, bg);
    ::DeleteObject(bg);
    ::DrawIconEx(dc, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);

    int stride = size * 4;
    DWORD imgSize = (DWORD)stride * size;
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + imgSize;
    BITMAPINFOHEADER ih = bi.bmiHeader;
    ih.biHeight = -size;
    ih.biSizeImage = imgSize;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) return false;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(bits, 1, imgSize, f);
    fclose(f);

    ::SelectObject(dc, old);
    ::DeleteObject(bmp);
    ::DeleteDC(dc);
    ::ReleaseDC(nullptr, screen);
    return true;
}

static int gid = 101;

int main() {
    int argc = 0;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (argc < 2) {
        printf("usage: icotest <exe> [groupid] [outprefix]\n");
        return 1;
    }
    if (argc >= 3) gid = _wtoi(argv[2]);
    HMODULE h = nullptr;
    const DWORD flags[] = {LOAD_LIBRARY_AS_DATAFILE, LOAD_LIBRARY_AS_IMAGE_RESOURCE, 0};
    const char* names[] = {"AS_DATAFILE", "AS_IMAGE_RESOURCE", "normal"};
    for (int i = 0; i < 3 && !h; ++i) {
        h = ::LoadLibraryExW(argv[1], nullptr, flags[i]);
        if (!h)
            printf("  %s: failed %lu\n", names[i], ::GetLastError());
        else if (::FindResourceW(h, MAKEINTRESOURCEW(gid), MAKEINTRESOURCEW(14)))
            printf("  %s: 资源可访问\n", names[i]);
        else {
            printf("  %s: 无资源 (%lu)，换下一种\n", names[i], ::GetLastError());
            ::FreeLibrary(h);
            h = nullptr;
        }
    }
    if (!h) {
        printf("三种方式都读不到图标资源\n");
        return 2;
    }
    HRSRC grp = ::FindResourceW(h, MAKEINTRESOURCEW(gid), MAKEINTRESOURCEW(14));
    printf("RT_GROUP_ICON(101) = %s\n", grp ? "FOUND" : "MISSING");
    int found = 0;
    for (int id = 1; id <= 8; ++id) {
        HRSRC r = ::FindResourceW(h, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(3));
        if (r) ++found;
    }
    printf("RT_ICON entries = %d\n", found);

    HICON big = (HICON)::LoadImageW(h, MAKEINTRESOURCEW(gid), IMAGE_ICON, 0, 0,
                                    LR_DEFAULTCOLOR | LR_DEFAULTSIZE);
    HICON small = (HICON)::LoadImageW(h, MAKEINTRESOURCEW(gid), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    printf("LoadImage 32=%p 16=%p\n", (void*)big, (void*)small);
    if (!big) {
        return 3;
    }
    const wchar_t* prefix = argc > 3 ? argv[3] : L"build/icon_out";
    wchar_t p1[MAX_PATH], p2[MAX_PATH];
    ::wsprintfW(p1, L"%s_32.bmp", prefix);
    ::wsprintfW(p2, L"%s_256.bmp", prefix);
    SaveBmp(p1, big, 32);
    HICON huge = (HICON)::LoadImageW(h, MAKEINTRESOURCEW(gid), IMAGE_ICON, 256, 256, LR_DEFAULTCOLOR);
    SaveBmp(p2, huge ? huge : big, 256);
    ::DestroyIcon(huge);
    ::DestroyIcon(big);
    if (small) ::DestroyIcon(small);
    ::FreeLibrary(h);
    printf("OK\n");
    if (argv) ::LocalFree(argv);
    return 0;
}
