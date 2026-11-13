// 五笔拼音混输的引擎级验证（不依赖 TSF，直接喂字母给 Engine）
//
// 验证点：
//   1) 混输关闭时行为与以前完全一致（五笔候选、四码唯一上屏）
//   2) 混输开启时，五笔打不出的串能按拼音出候选，且五笔候选仍排在前面
//   3) 混输开启时，第五码起按拼音继续拼（不会被“五码上屏”截断）
//   4) 混输开启时，四码唯一仍然自动上屏
#include <windows.h>
#include <string>
#include <vector>
#include "../core/qy_engine.h"

using namespace qy;

static std::wstring g_out;
static int g_pass = 0, g_fail = 0;

static void Check(const wchar_t* name, bool ok, const std::wstring& detail = L"") {
    wchar_t buf[512];
    ::wsprintfW(buf, L"%s %s%s", ok ? L"[PASS]" : L"[FAIL]", name, detail.empty() ? L"" : L"  ");
    g_out += buf;
    if (!detail.empty()) g_out += detail;
    g_out += L"\r\n";
    if (ok)
        ++g_pass;
    else
        ++g_fail;
}

static std::wstring CandLine(Engine& e, int n = 8) {
    std::wstring s;
    const auto& cs = e.cands();
    for (int i = 0; i < n && i < (int)cs.size(); ++i) {
        wchar_t b[128];
        ::wsprintfW(b, L"%d:%s(%hs,r%d) ", i + 1, cs[i].text.c_str(), cs[i].code.c_str(),
                    cs[i].rank);
        s += b;
    }
    return s;
}

static bool HasText(Engine& e, const wchar_t* t) {
    for (auto& c : e.cands())
        if (c.text == t) return true;
    return false;
}

static int IndexOf(Engine& e, const wchar_t* t) {
    const auto& cs = e.cands();
    for (int i = 0; i < (int)cs.size(); ++i)
        if (cs[i].text == t) return i;
    return -1;
}

// 打一串字母（自动跳过五码上屏造成的“缓冲清空”，模拟真实按键）
static std::wstring Type(Engine& e, const char* s, std::wstring* bufOut = nullptr) {
    std::wstring commit;
    for (const char* p = s; *p; ++p) {
        e.OnLetter(*p);
        std::wstring c;
        while (e.TakeCommit(c)) commit += c;
    }
    if (bufOut) *bufOut = e.bufferW();
    return commit;
}

static void Reset(Engine& e, bool mixed) {
    Options o;
    o.gbkOnly = 1;
    e.SetOptions(o);
    e.Clear();
    std::wstring drop;
    while (e.TakeCommit(drop)) {
    }
    e.SetMode(mixed ? Mode::Mixed : Mode::Wubi);
}

// 纯拼音方案（第三个方案是独立的，拼音本身不能被混输改动）
static void ResetPinyin(Engine& e) {
    Options o;
    o.gbkOnly = 1;
    e.SetOptions(o);
    e.Clear();
    std::wstring drop;
    while (e.TakeCommit(drop)) {
    }
    e.SetMode(Mode::Pinyin);
}

int main() {
    std::wstring dir = L"data";
    Engine e;
    if (!e.Init(dir)) {
        ::MessageBoxW(nullptr, L"码表 data\\qywb.dat 载入失败（请在 qywb 目录下运行）", L"mixtest",
                      MB_OK);
        return 2;
    }

    // ---- 1. 混输关闭：回到“纯五笔” ----
    {
        Reset(e, false);
        std::wstring buf;
        Type(e, "niha", &buf);
        Check(L"[1] 混输关闭：niha 后缓冲仍是四码", buf == L"niha", buf);
        Check(L"[2] 混输关闭：没有拼音候选（候选数==五笔候选数）",
              e.wubiCount() == e.cands().size(),
              L"wubi=" + std::to_wstring(e.wubiCount()) + L" all=" +
                  std::to_wstring(e.cands().size()));
        g_out += L"     " + CandLine(e) + L"\r\n";
        Check(L"[3] 混输关闭：候选里没有“你好”", !HasText(e, L"你好"));
    }

    // ---- 2. 混输开启：拼音兜底 ----
    {
        Reset(e, true);
        std::wstring buf;
        Type(e, "niha", &buf);
        Check(L"[4] 混输开启：niha 后缓冲仍是四码", buf == L"niha", buf);
        Check(L"[5] 混输开启：候选里出现“你好”（拼音）", HasText(e, L"你好"), CandLine(e));
        Check(L"[6] 混输开启：名次单调（0 精确 → 1 五笔前缀 → 2 拼音前缀）", [&] {
            int prev = -1;
            bool hasPyPrefix = false;
            for (auto& c : e.cands()) {
                if (c.rank < prev) return false;
                prev = c.rank;
                if (c.rank >= 2) hasPyPrefix = true;
            }
            return hasPyPrefix;
        }(), CandLine(e));
    }

    // ---- 3. 混输开启：第五码起按拼音继续拼 ----
    {
        Reset(e, true);
        std::wstring buf;
        std::wstring commit = Type(e, "nihao", &buf);
        Check(L"[7] 混输开启：nihao 能连续输入（缓冲=nihao）", buf == L"nihao", buf);
        Check(L"[8] 混输开启：nihao 没有被“五码上屏”截断", commit.empty(), commit);
        Check(L"[9] 混输开启：nihao 首选就是“你好”", !e.cands().empty() &&
                  e.cands()[0].text == L"你好", CandLine(e));
        // 空格上屏
        e.OnSpace();
        std::wstring out;
        e.TakeCommit(out);
        Check(L"[10] 混输开启：空格把“你好”上屏", out == L"你好", out);
    }

    // ---- 4. 混输开启：四码唯一仍然自动上屏 ----
    // 先挑一条“真的唯一”的四码（多数字有两个以上候选，不能凭印象挑），
    // 再用同一条码对比“混输开/关”的上屏结果是否一致
    {
        const char* codes[] = {"gggg", "hhhh", "jjjj", "llll", "nnnn", "ssss", "dddd",
                               "ffff", "vvvv", "bbbb", "xxxx", "cccc", "qqqq", "wwww",
                               "yyyy", "pppp", "tttt", "rrrr", "eeee", "uuuu", "iiii",
                               "oooo", "aaaa", "mmmm"};
        std::string uniq;
        std::wstring uniqCommit;
        for (const char* c : codes) {
            Reset(e, false);
            std::wstring buf;
            std::wstring commit = Type(e, c, &buf);
            if (!commit.empty() && buf.empty()) {  // 四码打满就自动上屏了 → 唯一
                uniq = c;
                uniqCommit = commit;
                break;
            }
        }
        Check(L"[11] 找到一条四码唯一的编码", !uniq.empty(),
              uniq.empty() ? L"" : std::wstring(uniq.begin(), uniq.end()) + L" -> " + uniqCommit);
        if (!uniq.empty()) {
            Reset(e, true);
            std::wstring buf;
            std::wstring commit = Type(e, uniq.c_str(), &buf);
            Check(L"[12] 混输开启：四码唯一依旧自动上屏（与纯五笔结果一致）",
                  commit == uniqCommit && buf.empty(),
                  L"commit=【" + commit + L"】buf=" + buf);
            // 顺便确认“五笔码唯一但拼音也有候选”的情况下不会误判成唯一
            Reset(e, true);
            Type(e, "niha", &buf);
            // niha 没有任何五笔码匹配，只有拼音候选：不能因为“只有 1 个候选”就上屏
            Check(L"[13] 混输开启：只有拼音候选时不自动上屏（缓冲保留）",
                  buf == L"niha" && e.wubiCount() == 0 && !e.cands().empty(),
                  L"buf=" + buf + L" wubi=" + std::to_wstring(e.wubiCount()));
        }
    }

    // ---- 5. 混输关闭：五码上屏首选（老行为） ----
    {
        Reset(e, false);
        std::wstring buf;
        std::wstring commit = Type(e, "wqiyw", &buf);
        Check(L"[14] 混输关闭：第五码仍按老规则上屏首选并重新开始", commit == L"你" && buf == L"w",
              L"commit=【" + commit + L"】buf=" + buf);
    }

    // ---- 6. 混输开启：五笔码打不出时的典型场景 ----
    {
        Reset(e, true);
        std::wstring buf;
        Type(e, "nihao", &buf);
        // 换成“忘了码”的另一串：haoma（号码）
        Reset(e, true);
        Type(e, "haoma", &buf);
        Check(L"[15] 混输开启：haoma 能出中文候选", !e.cands().empty(), CandLine(e));
        g_out += L"     haoma -> " + CandLine(e) + L"\r\n";
    }

    // ---- 6b. 三个方案互相独立：纯五笔里不该有拼音，纯拼音里不该有五笔 ----
    {
        Reset(e, false);
        std::wstring buf;
        Type(e, "niha", &buf);
        Check(L"[24] 五笔方案：打 niha 不出任何候选（不掺拼音）", e.cands().empty(),
              L"wubi=" + std::to_wstring(e.wubiCount()) + L" all=" +
                  std::to_wstring(e.cands().size()));
        ResetPinyin(e);
        Type(e, "nihao", &buf);
        Check(L"[25] 拼音方案：nihao 首选“你好”", !e.cands().empty() && e.cands()[0].text == L"你好",
              CandLine(e));
        Check(L"[26] 拼音方案：候选全部来自拼音（没有五笔候选）", e.wubiCount() == 0,
              L"wubi=" + std::to_wstring(e.wubiCount()));
        ResetPinyin(e);
        Type(e, "hhhh", &buf);
        Check(L"[27] 拼音方案：hhhh 不触发四码唯一上屏", buf == L"hhhh", L"buf=" + buf);
        ResetPinyin(e);
        Type(e, "ni", &buf);
        Check(L"[28] 拼音方案：ni 首选“你”", !e.cands().empty() && e.cands()[0].text == L"你",
              CandLine(e));
    }

    // ---- 7. 混输真实可用性：拼音候选必须出现在第一页（否则用户眼里就是“没实现”） ----
    {
        struct Case {
            const char* code;
            const wchar_t* want;
        };
        const Case cases[] = {{"ni", L"你"}, {"hao", L"好"}, {"wo", L"我"}, {"shi", L"是"}};
        for (auto& c : cases) {
            Reset(e, true);
            std::wstring buf;
            Type(e, c.code, &buf);
            int idx = IndexOf(e, c.want);
            wchar_t name[160];
            ::wsprintfW(name, L"[17] 混输开启：打 %hs 第一页就能看到“%s”", c.code, c.want);
            Check(name, idx >= 0 && idx < 5, L"idx=" + std::to_wstring(idx) + L"  " + CandLine(e));
        }
    }

    // ---- 8. 不能再出现“四码被五笔前缀抢上屏，拼音断掉”（shiy → shiyan） ----
    {
        Reset(e, true);
        std::wstring buf, commit = Type(e, "shiy", &buf);
        Check(L"[18] 混输开启：shiy 不自动上屏（否则打不下去 shiyan）",
              commit.empty() && buf == L"shiy", L"commit=【" + commit + L"】buf=" + buf);
        Reset(e, true);
        Type(e, "shiyan", &buf);
        Check(L"[19] 混输开启：shiyan 出“实验/试验”", HasText(e, L"实验") || HasText(e, L"试验"),
              CandLine(e));
        Reset(e, true);
        Type(e, "xiexie", &buf);
        Check(L"[20] 混输开启：xiexie 首选“谢谢”", !e.cands().empty() && e.cands()[0].text == L"谢谢",
              CandLine(e));
        Reset(e, true);
        Type(e, "ziji", &buf);
        Check(L"[21] 混输开启：ziji 首选“自己”", !e.cands().empty() && e.cands()[0].text == L"自己",
              CandLine(e));
    }

    // ---- 9. 纯五笔不能被混输改动：一级简码、四码唯一照旧 ----
    {
        Reset(e, false);
        std::wstring buf;
        Type(e, "n", &buf);
        Check(L"[22] 混输关闭：单字母一级简码首选“民”", !e.cands().empty() && e.cands()[0].text == L"民",
              CandLine(e));
        Reset(e, false);
        Type(e, "shiy", &buf);
        Check(L"[23] 混输关闭：shiy 四码唯一仍自动上屏", buf.empty(), L"buf=" + buf);
    }

    // ---- 10. 性能：混输每个键多查一次拼音，量一下最坏情况（每串总耗时） ----
    {
        LARGE_INTEGER f, a, b;
        ::QueryPerformanceFrequency(&f);
        auto Bench = [&](const char* str, bool mixed) -> double {
            Reset(e, mixed);
            const int N = 100;
            ::QueryPerformanceCounter(&a);
            for (int i = 0; i < N; ++i) {
                e.Clear();
                std::wstring drop;
                while (e.TakeCommit(drop)) {
                }
                for (const char* p = str; *p; ++p) e.OnLetter(*p);
            }
            ::QueryPerformanceCounter(&b);
            return (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart / N;
        };
        const char* cases[] = {"s", "z", "zhong", "nihao", "wqiy"};
        for (const char* c : cases) {
            double off = Bench(c, false);
            double on = Bench(c, true);
            // wsprintfW 不支持浮点，换算成微秒整数再打印
            int offUs = (int)(off * 1000.0 + 0.5), onUs = (int)(on * 1000.0 + 0.5);
            wchar_t b2[256];
            ::wsprintfW(b2, L"     %hs: 关 %d.%03d ms / 开 %d.%03d ms", c, offUs / 1000,
                        offUs % 1000, onUs / 1000, onUs % 1000);
            g_out += std::wstring(b2) + L"\r\n";
            // 混输不应把每次按键拖慢到能感觉出来的程度（一串输入 < 8ms）
            if (on > 8.0) {
                Check(L"[16] 混输性能可接受", false, b2);
            }
        }
        Check(L"[16] 混输性能可接受（每串输入 < 8ms）", true);
    }

    // ---- 3. 自定义词条：网络用语 / APP 名称 ----
    // 码按约定生成：两字词=各字五笔码前两位（微信 tmwy、豆包 gkqn、夸克 dfdq），
    // 3~4 字=拼音首字母（永远的神 yyds、小红书 xhs）。
    {
        struct Case {
            const char* code;
            const wchar_t* text;
        };
        const Case kCases[] = {
            {"tmwy", L"微信"}, {"gkqn", L"豆包"}, {"dfdq", L"夸克"},
            {"xhs", L"小红书"}, {"yyds", L"永远的神"}, {"yiai", L"京东"},
        };
        int n = 30;
        for (const auto& cs : kCases) {
            // 关掉“四码唯一自动上屏”，否则像 yyds 这种唯一码会直接上屏，
            // 候选列表已清空，反而看不到词条是否在库里。
            Options o;
            o.gbkOnly = 1;
            o.autoUnique4 = 0;
            e.SetOptions(o);
            e.Clear();
            std::wstring drop;
            while (e.TakeCommit(drop)) {
            }
            e.SetMode(Mode::Wubi);
            Type(e, cs.code);
            int idx = IndexOf(e, cs.text);
            wchar_t name[128];
            ::wsprintfW(name, L"[%d] 五笔自定义码 %hs → %s", n++, cs.code, cs.text);
            Check(name, idx >= 0, CandLine(e));
        }
        // 拼音/简拼同样能找到
        ResetPinyin(e);
        Type(e, "wx");
        Check(L"[36] 拼音简拼 wx → 微信", HasText(e, L"微信"), CandLine(e));
        ResetPinyin(e);
        Type(e, "yyds");
        Check(L"[37] 拼音简拼 yyds → 永远的神", HasText(e, L"永远的神"), CandLine(e));
    }

    // ---- 11. 临时拼音（五笔方案下按 z 进入，输入结束自动恢复五笔） ----
    {
        // ① 引导条件：只有「五笔 + 空编码 + 尚未进入」才把 z 当功能键
        Reset(e, false);
        Check(L"[40] 五笔空编码：z 可作临时拼音引导键", e.CanTempPinyin());
        ResetPinyin(e);
        Check(L"[41] 拼音方案不提供 z 引导（z 仍是普通字母）", !e.CanTempPinyin());
        Reset(e, true);
        Check(L"[42] 混输方案不提供 z 引导", !e.CanTempPinyin());

        // ② 进入后方案不变、编码为空（状态条才临时显示「拼」）
        Reset(e, false);
        e.BeginTempPinyin();
        Check(L"[43] 进入临时拼音：仍是五笔方案，编码为空",
              e.tempPinyin() && e.mode() == Mode::Wubi && !e.hasComp());
        Check(L"[44] 已进入后 z 不再当引导键（要留给 zhong / zai）", !e.CanTempPinyin());

        // ③ z 开头音节：引导键之后的第二个 z 是普通拼音字母
        std::wstring buf;
        Type(e, "zhong", &buf);
        Check(L"[45] 临时拼音：zhong 完整保留在编码缓冲", buf == L"zhong", buf);
        Check(L"[46] 临时拼音：zhong 出拼音候选「中」", HasText(e, L"中"), CandLine(e));
        Check(L"[47] 临时拼音：没有五笔候选（按拼音查库）", e.wubiCount() == 0,
              L"wubi=" + std::to_wstring(e.wubiCount()));

        // ④ 普通全拼 + 上屏后自动恢复五笔
        Reset(e, false);
        e.BeginTempPinyin();
        Type(e, "nihao", &buf);
        Check(L"[48] 临时拼音：nihao 首选「你好」",
              !e.cands().empty() && e.cands()[0].text == L"你好", CandLine(e));
        e.OnSpace();
        std::wstring out;
        e.TakeCommit(out);
        Check(L"[49] 临时拼音：空格把「你好」上屏", out == L"你好", out);
        Check(L"[50] 输入结束：临时拼音自动复位、方案仍是五笔",
              !e.tempPinyin() && e.mode() == Mode::Wubi && !e.hasComp());
        Type(e, "niha", &buf);
        Check(L"[51] 恢复五笔后：niha 不再出拼音候选", e.cands().empty(),
              L"wubi=" + std::to_wstring(e.wubiCount()) + L" all=" +
                  std::to_wstring(e.cands().size()));

        // ⑤ Esc / 回车 / 主动取消 / 切方案，四条结束路径都要复位
        Reset(e, false);
        e.BeginTempPinyin();
        Type(e, "ni", &buf);
        e.OnEscape();
        Check(L"[52] Esc 退出临时拼音并清空编码",
              !e.tempPinyin() && !e.hasComp() && e.mode() == Mode::Wubi);
        Reset(e, false);
        e.BeginTempPinyin();
        Type(e, "ni", &buf);
        e.OnEnter();
        Check(L"[53] 回车清屏退出临时拼音",
              !e.tempPinyin() && !e.hasComp() && e.mode() == Mode::Wubi);
        Reset(e, false);
        e.BeginTempPinyin();
        Type(e, "ni", &buf);
        e.CancelTempPinyin();
        Check(L"[54] 主动取消临时拼音（状态条点「拼」/ 切窗口）",
              !e.tempPinyin() && !e.hasComp() && e.mode() == Mode::Wubi);
        Reset(e, false);
        e.BeginTempPinyin();
        e.SetMode(Mode::Mixed);
        Check(L"[55] 切方案时临时拼音一并复位", !e.tempPinyin() && e.mode() == Mode::Mixed);

        // ⑥ 四码唯一自动上屏不能误伤临时拼音
        Reset(e, false);
        e.BeginTempPinyin();
        std::wstring c4 = Type(e, "ggtt", &buf);
        Check(L"[56] 临时拼音：四码不触发五笔唯一上屏", c4.empty() && buf == L"ggtt",
              L"commit=【" + c4 + L"】buf=" + buf);
    }

    ::OutputDebugStringW(g_out.c_str());
    wchar_t path[MAX_PATH];
    ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p = path;
    size_t sl = p.find_last_of(L'\\');
    p = p.substr(0, sl) + L"\\mixtest.txt";
    HANDLE h = ::CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        std::wstring head;
        wchar_t b[128];
        ::wsprintfW(b, L"PASS=%d FAIL=%d\r\n", g_pass, g_fail);
        head = b;
        std::string u8;
        int n = ::WideCharToMultiByte(CP_UTF8, 0, (head + g_out).c_str(), -1, nullptr, 0, nullptr,
                                     nullptr);
        u8.resize(n);
        ::WideCharToMultiByte(CP_UTF8, 0, (head + g_out).c_str(), -1, &u8[0], n, nullptr, nullptr);
        DWORD wr = 0;
        ::WriteFile(h, u8.c_str(), (DWORD)u8.size() - 1, &wr, nullptr);
        ::CloseHandle(h);
    }
    return g_fail == 0 ? 0 : 1;
}
