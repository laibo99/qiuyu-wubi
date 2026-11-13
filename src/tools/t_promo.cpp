// 手动调频（Ctrl+数字）引擎级测试：
// ① 把第 2 个候选调到首位；② 重建引擎后顺序仍保持（持久化）；③ 清除记录后恢复默认。
#include <windows.h>
#include <stdio.h>
#include <string>
#include <vector>
#include "../core/qy_engine.h"

using namespace qy;

static FILE* g_f = nullptr;

static void LogU8(const std::string& s) {
    if (g_f) fwrite(s.data(), 1, s.size(), g_f);
}

static void Line(const std::wstring& w) { LogU8(WToUtf8(w) + "\n"); }

static std::wstring List(Engine& e, int maxN) {
    std::wstring out;
    int n = 0;
    for (auto& c : e.cands()) {
        if (n >= maxN) break;
        if (n) out += L" | ";
        out += std::to_wstring(n + 1) + L"." + c.text;
        ++n;
    }
    return out;
}

static void Type(Engine& e, const char* code) {
    for (const char* p = code; *p; ++p) e.OnLetter(*p);
}

static int g_fail = 0;
static void Check(bool ok, const std::wstring& name) {
    Line((ok ? L"[PASS] " : L"[FAIL] ") + name);
    if (!ok) ++g_fail;
}

int main() {
    // 用独立的用户目录，避免污染真实配置
    wchar_t tmp[MAX_PATH];
    ::GetTempPathW(MAX_PATH, tmp);
    std::wstring udir = std::wstring(tmp) + L"qywb_promo_test";
    ::SHCreateDirectoryExW(nullptr, udir.c_str(), nullptr);
    ::SetEnvironmentVariableW(L"QYWB_USERDIR", udir.c_str());
    // 清掉上次测试残留
    ::DeleteFileW((udir + L"\\promote.dat").c_str());

    fopen_s(&g_f, "build/promo_out.txt", "wb");
    if (!g_f) return 2;

    Options o;
    o.manualFreq = 1;
    o.gbkOnly = 1;

    Engine e1;
    e1.SetOptions(o);
    if (!e1.Init(L"data")) {
        Line(L"[FAIL] 字库加载失败");
        fclose(g_f);
        return 3;
    }

    Type(e1, "gg");
    std::wstring before = List(e1, 5);
    Line(L"调频前 gg: " + before);
    Check(e1.cands().size() >= 2, L"gg 至少有两个候选");
    if (e1.cands().size() < 2) {
        fclose(g_f);
        return 4;
    }
    std::wstring second = e1.cands()[1].text;

    // 把第 2 个候选调到第 1 位
    Check(e1.PromoteOnPage(1), L"PromoteOnPage(1) 返回成功");
    std::wstring after = List(e1, 5);
    Line(L"调频后 gg: " + after);
    Check(!e1.cands().empty() && e1.cands()[0].text == second, L"第 2 个候选已升到第 1 位");
    Check(e1.cands().size() >= 2 && e1.cands()[1].text == L"五", L"原来的首位退到第 2 位");

    // 持久化：新引擎重新加载后仍是调频后的顺序
    Engine e2;
    e2.SetOptions(o);
    e2.Init(L"data");
    Type(e2, "gg");
    Line(L"重新加载 gg: " + List(e2, 5));
    Check(!e2.cands().empty() && e2.cands()[0].text == second, L"调频记录已持久化");

    // 其他编码不受影响
    Type(e2, "x");
    Line(L"其它编码 ggx: " + List(e2, 3));

    // 清除记录后恢复默认
    e2.ClearPromotions();
    Engine e3;
    e3.SetOptions(o);
    e3.Init(L"data");
    Type(e3, "gg");
    Line(L"清除后 gg: " + List(e3, 5));
    Check(!e3.cands().empty() && e3.cands()[0].text == L"五", L"清除记录后恢复默认顺序（五）");

    // 关闭手动调频时不允许调频
    Options off = o;
    off.manualFreq = 0;
    Engine e4;
    e4.SetOptions(off);
    e4.Init(L"data");
    Type(e4, "gg");
    Check(!e4.PromoteOnPage(1), L"关闭手动调频后 PromoteOnPage 被拒绝");

    Line(g_fail == 0 ? L"\n全部通过" : L"\n存在失败项");
    fclose(g_f);
    ::DeleteFileW((udir + L"\\promote.dat").c_str());
    return g_fail == 0 ? 0 : 1;
}
