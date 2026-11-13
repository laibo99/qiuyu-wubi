// 秋雨五笔输入法 - 图标生成器（GDI 绘制“秋”字，输出多尺寸 ICO）
#include <windows.h>
#include <stdio.h>
#include <vector>
#include <string>

#pragma pack(push, 1)
struct IcoDir { WORD reserved, type, count; };
struct IcoEntry { BYTE w, h, colors, reserved; WORD planes, bits; DWORD bytes, offset; };
#pragma pack(pop)

static HBITMAP MakeBitmap(HDC dc, int size, void** bits) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    return ::CreateDIBSection(dc, &bi, DIB_RGB_COLORS, bits, nullptr, 0);
}

static void DrawIcon(HDC dc, int size) {
    // 圆角背景 + 竖直渐变（深蓝 -> 亮蓝）
    for (int y = 0; y < size; ++y) {
        int t = y * 255 / (size - 1);
        int r = 24 + t * 20 / 255;
        int g = 78 + t * 60 / 255;
        int b = 150 + t * 70 / 255;
        HBRUSH br = ::CreateSolidBrush(RGB(r, g, b));
        RECT rc{0, y, size, y + 1};
        ::FillRect(dc, &rc, br);
        ::DeleteObject(br);
    }
    // “秋”字（右侧原有一枚雨滴装饰，已按要求去掉）
    LOGFONTW lf{};
    lf.lfHeight = -(int)(size * 0.62);
    lf.lfWeight = FW_BOLD;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Microsoft YaHei");
    HFONT f = ::CreateFontIndirectW(&lf);
    HFONT of = (HFONT)::SelectObject(dc, f);
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, RGB(255, 255, 255));
    const wchar_t* ch = L"秋";
    SIZE sz{};
    ::GetTextExtentPoint32W(dc, ch, 1, &sz);
    ::TextOutW(dc, (size - sz.cx) / 2, (size - sz.cy) / 2 - size / 20, ch, 1);
    ::SelectObject(dc, of);
    ::DeleteObject(f);
}

int main(int argc, char** argv) {
    const wchar_t* out = argc > 1 ? L"qywb.ico" : L"qywb.ico";
    int sizes[] = {16, 24, 32, 48, 64, 128, 256};
    int n = sizeof(sizes) / sizeof(sizes[0]);

    HDC screen = ::GetDC(nullptr);
    std::vector<std::vector<BYTE>> images;
    for (int i = 0; i < n; ++i) {
        int s = sizes[i];
        void* bits = nullptr;
        HBITMAP bmp = MakeBitmap(screen, s, &bits);
        if (!bmp) continue;
        HDC mem = ::CreateCompatibleDC(screen);
        HGDIOBJ old = ::SelectObject(mem, bmp);
        DrawIcon(mem, s);
        ::SelectObject(mem, old);
        ::DeleteDC(mem);

        // ICO 图像：BITMAPINFOHEADER(40) + 32bpp 自底向上 + AND 掩码
        int rowBytes = s * 4;
        int andRow = ((s + 31) / 32) * 4;
        std::vector<BYTE> img(40 + rowBytes * s + andRow * s, 0);
        BITMAPINFOHEADER* hdr = (BITMAPINFOHEADER*)img.data();
        hdr->biSize = 40;
        hdr->biWidth = s;
        hdr->biHeight = s * 2;
        hdr->biPlanes = 1;
        hdr->biBitCount = 32;
        hdr->biCompression = BI_RGB;
        hdr->biSizeImage = rowBytes * s;
        const BYTE* src = (const BYTE*)bits;
        for (int y = 0; y < s; ++y) {
            memcpy(img.data() + 40 + (s - 1 - y) * rowBytes, src + y * rowBytes, rowBytes);
        }
        ::DeleteObject(bmp);
        images.push_back(std::move(img));
    }
    ::ReleaseDC(nullptr, screen);

    IcoDir dir{0, 1, (WORD)images.size()};
    std::vector<IcoEntry> entries(images.size());
    DWORD offset = (DWORD)(sizeof(IcoDir) + sizeof(IcoEntry) * images.size());
    for (size_t i = 0; i < images.size(); ++i) {
        entries[i].w = (BYTE)(sizes[i] >= 256 ? 0 : sizes[i]);
        entries[i].h = entries[i].w;
        entries[i].colors = 0;
        entries[i].reserved = 0;
        entries[i].planes = 1;
        entries[i].bits = 32;
        entries[i].bytes = (DWORD)images[i].size();
        entries[i].offset = offset;
        offset += (DWORD)images[i].size();
    }
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, out, L"wb") != 0 || !fp) return 1;
    fwrite(&dir, sizeof(dir), 1, fp);
    fwrite(entries.data(), sizeof(IcoEntry), entries.size(), fp);
    for (auto& img : images) fwrite(img.data(), 1, img.size(), fp);
    fclose(fp);
    printf("生成 %ls (%u 尺寸)\n", out, (unsigned)images.size());
    return 0;
}
