// 候选顺序测试：模拟逐字母输入，打印前 6 个候选
#include <windows.h>
#include <stdio.h>
#include <string>
#include "../core/qy_engine.h"

using namespace qy;

static std::wstring Wide(const char* s) {
    wchar_t buf[64];
    MultiByteToWideChar(CP_UTF8, 0, s, -1, buf, 64);
    return buf;
}

int main() {
    Engine eng;
    Options o;
    eng.SetOptions(o);
    std::wstring dir = L"data";
    if (!eng.Init(dir)) {
        wprintf(L"Init 失败\n");
        return 1;
    }
    FILE* f = nullptr;
    fopen_s(&f, "build/cand_out.txt", "wb");
    if (!f) return 2;
    const char* cases[] = {"gg", "hh", "lg", "g", "h", "l", "aaa", "wq", "gkl"};
    for (const char* c : cases) {
        eng.Clear();
        for (const char* p = c; *p; ++p) eng.OnLetter(*p);
        std::wstring line;
        int n = 0;
        for (const auto& cand : eng.cands()) {
            if (n >= 6) break;
            line += cand.text;
            line += L' ';
            ++n;
        }
        fprintf(f, "[%s] ", c);
        std::string u8;
        int wl = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), nullptr, 0,
                                     nullptr, nullptr);
        u8.resize(wl);
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), &u8[0], wl, nullptr,
                            nullptr);
        fwrite(u8.data(), 1, u8.size(), f);
        fputc('\n', f);
    }
    fclose(f);
    return 0;
}
