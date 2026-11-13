// 临时诊断工具：打印混输候选项，便于定位“拼音候选看不到”的问题
#include <windows.h>
#include <string>
#include <vector>
#include "../core/qy_engine.h"

using namespace qy;

static std::string Narrow(const std::wstring& w) {
    std::string s;
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    s.resize(n);
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

int main() {
    Engine e;
    if (!e.Init(L"data")) return 2;
    Options o;
    o.gbkOnly = 1;
    e.SetOptions(o);
    e.SetMode(Mode::Mixed);

    const char* inputs[] = {"n", "ni", "nih", "niha", "nihao", "hao", "haom", "haoma",
                            "w", "wo", "wom", "wome", "women", "shi", "shiy", "shiyan",
                            "wosh", "woshi", "jint", "jintian", "xiex", "xiexie", "ziji"};
    std::string out;
    for (const char* s : inputs) {
        e.Clear();
        std::wstring drop;
        while (e.TakeCommit(drop)) {
        }
        for (const char* p = s; *p; ++p) e.OnLetter(*p);
        out += s;
        out += "  buf=";
        out += Narrow(e.bufferW());
        std::wstring cm;
        while (e.TakeCommit(cm)) {
            out += " [已上屏:";
            out += Narrow(cm);
            out += "]";
        }
        out += "  wubi=" + std::to_string(e.wubiCount());
        out += "  all=" + std::to_string(e.cands().size());
        out += "  -> ";
        for (size_t i = 0; i < e.cands().size() && i < 10; ++i) {
            out += Narrow(e.cands()[i].text);
            out += "(r" + std::to_string(e.cands()[i].rank) + ") ";
        }
        out += "\n";
    }
    HANDLE h = ::CreateFileW(L"build\\qyprobe.txt", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        ::WriteFile(h, out.data(), (DWORD)out.size(), &w, nullptr);
        ::CloseHandle(h);
    }
    return 0;
}
