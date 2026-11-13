// 引擎行为自测：五笔/拼音切换、回车清屏、翻页、标点
#include <cstdio>
#include <windows.h>
#include <string>
#include <string>
#include "../core/qy_engine.h"

using namespace qy;

static Engine g;
static int gFail = 0;

static void Type(const char* s) {
    for (const char* p = s; *p; ++p) g.OnLetter(*p);
}

static std::wstring Top(int n) {
    std::wstring r;
    int i = 0;
    for (auto& c : g.cands()) {
        if (i++ >= n) break;
        if (!r.empty()) r += L"/";
        r += c.text;
    }
    return r;
}

static FILE* g_log = nullptr;

static void Puts(const std::wstring& w) {
    if (!g_log) return;
    int n = (int)w.size();
    int need = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), n, nullptr, 0, nullptr, nullptr);
    std::string s((size_t)need, 0);
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), n, &s[0], need, nullptr, nullptr);
    fwrite(s.data(), 1, s.size(), g_log);
}

static void Check(const wchar_t* name, bool ok, const std::wstring& detail) {
    fprintf(g_log, "[%s] ", ok ? "PASS" : "FAIL");
    Puts(name);
    Puts(L"  ->  ");
    Puts(detail);
    fputs("\n", g_log);
    fflush(g_log);
    if (!ok) ++gFail;
}

int main() {
    g_log = fopen("build/te2_utf8.txt", "wb");
    if (!g_log) return 9;
    Options o;
    o.pageSize = 9;
    g.SetOptions(o);

    // 优先用本仓库的码表，其次安装目录（避免测试依赖安装状态）
    std::wstring dir = L"data";
    if (::GetFileAttributesW(L"data\\qywb.dat") == INVALID_FILE_ATTRIBUTES)
        dir = L"C:\\Program Files\\QiuYuWubi\\data";
    if (!g.Init(dir)) {
        fputs("Init failed\n", g_log); fflush(g_log);
        return 2;
    }

    // ---- 1. 五笔 ----
    g.SetMode(Mode::Wubi);
    Type("qiuyu");
    Check(L"五笔 qiuyu", g.hasComp(), Top(6));
    g.Clear();

    Type("sm");
    Check(L"五笔 sm", g.hasComp() && Top(3).find(L"机") != std::wstring::npos, Top(6));
    g.Clear();

    // ---- 2. 拼音（同一输入法内切换） ----
    g.SetMode(Mode::Pinyin);
    Type("qiuyu");
    std::wstring pyTop = Top(6);
    Check(L"拼音 qiuyu", pyTop.find(L"秋雨") != std::wstring::npos, pyTop);

    // 回车清屏：不输出任何字符，组合清空
    std::wstring commit;
    std::wstring stale;
    if (g.TakeCommit(stale)) {
        Puts(L"  (之前残留 pending：");
        Puts(stale);
        Puts(L")\n");
        fflush(g_log);
    }
    g.OnEnter();
    bool noCommit = !g.TakeCommit(commit) || commit.empty();
    Check(L"回车清屏（不残码）", noCommit && !g.hasComp(), commit.empty() ? L"(无上屏)" : commit);

    Type("qy");
    // 简拼候选按词频排序，秋雨不一定在前，但必须在全量候选中出现
    bool found = false;
    std::wstring all;
    size_t savedPage = g.pageNo();
    for (size_t p = 0; p < g.pageCount() && p < 12; ++p) {
        for (auto& c : g.cands()) {
            if (c.text == L"秋雨") found = true;
            if (all.size() < 120) { all += c.text; all += L" "; }
        }
        if (!g.NextPage()) break;
    }
    while (g.pageNo() > savedPage && g.PrevPage()) {}
    Check(L"拼音简拼 qy 含秋雨", found, all);
    g.Clear();

    Type("srf");
    Check(L"拼音 srf", g.hasComp(), Top(6));
    g.Clear();

    // ---- 3. 空格上屏首选 ----
    Type("sm");
    g.OnSpace();
    bool ok = g.TakeCommit(commit) && !commit.empty();
    Check(L"空格上屏", ok && !g.hasComp(), commit);

    // ---- 4. 数字选词 ----
    Type("sm");
    g.OnDigit(2);
    ok = g.TakeCommit(commit) && !commit.empty();
    Check(L"数字键选词", ok, commit);

    // ---- 5. 翻页 ----
    Type("a");
    size_t pc = g.pageCount();
    bool paged = false;
    if (pc > 1) {
        std::wstring first = Top(1);
        g.OnPageKey(true);
        paged = (g.pageNo() == 1);
        g.OnPageKey(false);
    }
    Check(L"翻页", pc <= 1 || paged, std::to_wstring((long long)pc) + L" 页");
    g.Clear();

    // ---- 6. 中文标点 ----
    Type("sm");
    g.OnSpace();
    g.TakeCommit(commit);
    std::wstring p1 = g.punct(L',', false);
    std::wstring p2 = g.punct(L'.', false);
    std::wstring q1 = g.punct('"', false);
    std::wstring q2 = g.punct('"', false);
    Check(L"中文标点", p1 == L"，" && p2 == L"。", p1 + p2 + q1 + q2);

    // ---- 6.5 全角转换 ----
    o.fullWidth = 1;
    g.SetOptions(o);
    {
        std::wstring w = g.ToWidth(L"ab1 ,");
        bool okw = w.size() == 5 && w[0] == 0xFF41 && w[1] == 0xFF42 && w[2] == 0xFF11 &&
                   w[3] == 0x3000 && w[4] == 0xFF0C;
        Check(L"全角转换", okw, w);
    }
    o.fullWidth = 0;
    g.SetOptions(o);

    // ---- 6.6 回车清掉残留 pending ----
    Type("qiuyu");  // 五笔第5码触发首选上屏，产生 pending
    std::wstring pend;
    bool hadPending = g.TakeCommit(pend);  // 先取走上屏内容
    Type("qiuy");                          // 再造组合
    g.OnEnter();
    std::wstring after;
    bool clean = !g.TakeCommit(after) && after.empty() && !g.hasComp();
    Check(L"回车清屏（含 pending）", clean, hadPending ? pend + L" | (后续无输出)" : L"无pending");
    (void)hadPending;

    // ---- 7. 切回五笔 ----
    g.SetMode(Mode::Wubi);
    Type("nh");
    Check(L"切回五笔 nh", g.hasComp(), Top(6));

    fprintf(g_log, "FAILED=%d\n", gFail); fclose(g_log);
    return gFail ? 1 : 0;
}
