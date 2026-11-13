// 秋雨五笔输入法 - 引擎命令行测试
#include "../core/qy_engine.h"
#include <cstdio>
#include <string>

using namespace qy;

static void PrintCands(Engine& e, const char* label) {
    printf("%-14s => ", label);
    const auto& c = e.cands();
    for (size_t i = 0; i < c.size() && i < 12; ++i) {
        printf("%s[%s] ", WToUtf8(c[i].text).c_str(), c[i].code.c_str());
    }
    if (c.empty()) printf("(无候选)");
    printf("\n");
}

int main(int argc, char** argv) {
    ::SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::wstring dir = (argc > 1 && argv[1][0] != '-') ? Utf8ToW(argv[1]) : L"../data";
    Engine e;
    Options o;
    o.pageSize = 9;
    e.SetOptions(o);
    if (!e.Init(dir)) {
        printf("码表载入失败: %s\n", WToUtf8(dir).c_str());
        return 1;
    }
    printf("码表载入成功，共 %u 条\n", (unsigned)e.dict().Count());

    // 固定测试
    const char* cases[] = {"a", "w", "q", "aaa", "aaaa", "gkld", "qiuyu", "qy", "wubi",
                           "qiuyuwubi", "nihao", "nh", "zhongguo", "zg", "aazz", "sm"};
    for (auto cs : cases) {
        e.Clear();
        e.SetMode(Mode::Wubi);
        for (const char* p = cs; *p; ++p) e.OnLetter(*p);
        PrintCands(e, cs);
    }
    // 拼音模式
    const char* pycases[] = {"qiu", "qiuyu", "qy", "wubi", "qiuyuwubi", "nihao", "nh",
                             "zhongguo", "zg", "shurufa", "srf", "qiuyu wubi"};
    for (auto cs : pycases) {
        e.Clear();
        e.SetMode(Mode::Pinyin);
        for (const char* p = cs; *p; ++p) {
            if (*p == ' ') continue;
            e.OnLetter(*p);
        }
        PrintCands(e, cs);
    }

    // 交互：从标准输入读取编码
    if (argc <= 2) {
        printf("\n输入编码回车查看候选（q 退出）：\n");
        char line[256];
        while (fgets(line, sizeof(line), stdin)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            if (s == "q") break;
            if (s.empty()) continue;
            e.Clear();
            for (char ch : s) {
                if (ch >= 'a' && ch <= 'z') e.OnLetter(ch);
                else if (ch == ' ') {
                    e.OnSpace();
                    std::wstring c;
                    if (e.TakeCommit(c)) printf("  [上屏] %s\n", WToUtf8(c).c_str());
                } else if (ch >= '1' && ch <= '9') {
                    e.OnDigit(ch - '0');
                    std::wstring c;
                    if (e.TakeCommit(c)) printf("  [上屏] %s\n", WToUtf8(c).c_str());
                }
            }
            PrintCands(e, s.c_str());
        }
    }
    return 0;
}
