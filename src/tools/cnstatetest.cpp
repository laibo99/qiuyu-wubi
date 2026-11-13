// GHO 精简 Win10「点开设置后状态条中英无限循环」回归。
//
// 根因：每个 CTextService 实例的 _chinese 各存一份；设置窗口打开时系统重置
// GUID_COMPARTMENT_KEYBOARD_OPENCLOSE，多个实例收到 OnChange 后各自把隔间
// 写回自己的值 → A 写「中」、B 写「英」→ 互相触发 → 无限乒乓。
// 这里验证两条修复后的不变量：
//   ① 收到隔间通知时 writeBack 恒为 false（掐断乒乓）；
//   ② 中英有跨实例唯一真值，任何实例切换其它实例都能读到（画面不再一「中」一「英」）。
// ② 还守着一个踩过的坑：file mapping 句柄必须保留到 UnmapViewOfFile 之后，
//    否则第二次同名 CreateFileMappingW 会创建新对象，共享静默失效。
#include <cstdio>
#include <cstdlib>
#include <string>
#include <windows.h>
#include "../core/qy_common.h"

static int gFail = 0;
static FILE* gOut = nullptr;

static void Check(const char* name, bool ok, const char* detail = "") {
    printf("[%s] %s %s\n", ok ? "PASS" : "FAIL", name, detail);
    fflush(stdout);
    if (gOut) fprintf(gOut, "[%s] %s %s\n", ok ? "PASS" : "FAIL", name, detail);
    if (!ok) ++gFail;
}

int main(int argc, char** argv) {
    // 子进程模式：只往共享状态写一个值就退出（供父进程验证跨进程可见性）。
    // 必须放在打开结果文件之前，否则子进程会把父进程的结果文件冲掉。
    if (argc >= 3 && strcmp(argv[1], "--set") == 0) {
        qy::ChineseState cs =
            qy::OpenChineseStateNamed(L"Local\\QiuYuWubiCN_xproc_v1");
        qy::SetSharedChinese(cs, atoi(argv[2]) == 1);
        qy::CloseChineseState(cs);
        return 0;
    }

    gOut = fopen("build/cnstatetest.txt", "w");

    // ---- 1. 决策矩阵：所有组合（含 Win7 分支）都绝不允许写回隔间 ----
    int combos = 0, badWriteBack = 0;
    for (int inited = 0; inited < 2; ++inited)
        for (int grace = 0; grace < 2; ++grace)
            for (int ctrl = 0; ctrl < 2; ++ctrl)
                for (int key = 0; key < 2; ++key)
                    for (int opt = 0; opt < 2; ++opt)
                        for (int cool = 0; cool < 2; ++cool)
                            for (int pre10 = 0; pre10 < 2; ++pre10) {
                                auto d = qy::DecideCompartmentChange(
                                    inited != 0, grace != 0, ctrl != 0, key != 0, opt != 0,
                                    cool != 0, pre10 != 0);
                                ++combos;
                                if (d.writeBack) ++badWriteBack;
                            }
    Check("全部 128 种组合（含 Win7）都绝不写回隔间",
          combos == 128 && badWriteBack == 0, "");

    // ---- 2. 系统噪声一律不跟随（GHO 点开设置触发的就是这一类）----
    Check("系统重置隔间(无按键)→不跟随",
          !qy::DecideCompartmentChange(true, false, false, false, true, false).follow, "");
    Check("未初始化→不跟随",
          !qy::DecideCompartmentChange(false, false, true, true, true, false).follow, "");
    Check("激活宽限内→不跟随",
          !qy::DecideCompartmentChange(true, true, true, true, true, false).follow, "");
    Check("跟随冷却内→不跟随",
          !qy::DecideCompartmentChange(true, false, true, true, true, true).follow, "");
    Check("关闭 Ctrl+空格 选项→不跟随",
          !qy::DecideCompartmentChange(true, false, true, true, false, false).follow, "");

    // ---- 3. 真 Ctrl+空格 仍然要能切（别把功能改没了）----
    Check("按着 Ctrl→跟随",
          qy::DecideCompartmentChange(true, false, true, false, true, false).follow, "");
    Check("刚按过 Ctrl(松手快)→跟随",
          qy::DecideCompartmentChange(true, false, false, true, true, false).follow, "");

    // ---- 3b. Win7/8：Ctrl+空格 是系统热键，按键事件根本到不了 sink ----
    // 11-05 回归：上一轮把跟随条件收紧成「必须按着 Ctrl / 400ms 内按过 Ctrl」，
    // 且 _modKeyTick 记在 OnKeyDown(VK_CONTROL)（该键永不被吃，恒不更新），
    // 结果 Win7 上 Ctrl+空格 完全失效，只剩点状态条能切。Win7 的 TSF 不会在
    // 焦点切换时乱重置隔间，放宽跟随是安全的；乒乓的根（写回）依然掐死。
    Check("Win7 无按键证据→仍跟随（隔间是唯一通路）",
          qy::DecideCompartmentChange(true, false, false, false, true, false, true).follow, "");
    Check("Win7 未初始化→仍不跟随",
          !qy::DecideCompartmentChange(false, false, false, false, true, false, true).follow, "");
    Check("Win7 激活宽限内→仍不跟随",
          !qy::DecideCompartmentChange(true, true, false, false, true, false, true).follow, "");
    Check("Win7 跟随冷却内→仍不跟随",
          !qy::DecideCompartmentChange(true, false, false, false, true, true, true).follow, "");
    Check("Win7 关闭 Ctrl+空格 选项→仍不跟随",
          !qy::DecideCompartmentChange(true, false, false, false, false, false, true).follow, "");
    Check("Win10 无按键证据→不跟随（GHO 噪声判据保持不变）",
          !qy::DecideCompartmentChange(true, false, false, false, true, false, false).follow, "");
    Check("本机真实系统版本与 preWin10 判据自洽",
          qy::IsPreWin10() == (qy::RealOsMajorVersion() < 10) &&
              qy::RealOsMajorVersion() >= 6, "");

    // ---- 4. 乒乓场景：A=中、B=英，互相收到对方写的值都不许响应 ----
    {
        auto a = qy::DecideCompartmentChange(true, false, false, false, true, false);
        auto b = qy::DecideCompartmentChange(true, false, false, false, true, false);
        Check("A(中)收到 open=0→不写回不跟随", !a.writeBack && !a.follow, "");
        Check("B(英)收到 open=1→不写回不跟随", !b.writeBack && !b.follow, "");
        Check("乒乓链就此终止(不会生成第二轮事件)", !a.writeBack && !b.writeBack, "");
    }

    // ---- 5. 跨实例唯一中英真值（用测试专用映射名，不碰真实输入法状态）----
    const wchar_t* kName = L"Local\\QiuYuWubiCN_selftest_v1";
    qy::ChineseState a = qy::OpenChineseStateNamed(kName);
    qy::ChineseState b = qy::OpenChineseStateNamed(kName);
    Check("两个独立实例都拿到同一份共享状态", a.view != nullptr && b.view != nullptr, "");
    Check("两个实例都持有有效 mapping 句柄", a.map != nullptr && b.map != nullptr, "");
    if (a.view && b.view) {
        Check("magic 已初始化",
              a.view->magic == 0x51594E42L && b.view->magic == 0x51594E42L, "");
        ::InterlockedExchange(&a.view->chinese, 0);  // 干净起点（上次跑可能留了值）
        Check("初始为未知 0", qy::SharedChinese(b) == 0, "");
        qy::SetSharedChinese(a, true);  // 实例 A 切中文
        Check("A 写中文→B 读到 1（共享真生效）", qy::SharedChinese(b) == 1, "");
        qy::SetSharedChinese(b, false);  // 实例 B 切英文
        Check("B 写英文→A 读到 2", qy::SharedChinese(a) == 2, "");
        const bool aChinese = qy::SharedChinese(a) == 1;
        Check("A 按共享真值静默同步为英文(不写隔间)", !aChinese, "");
        qy::SetSharedChinese(a, true);
        Check("A 再切中文→B 立即可见", qy::SharedChinese(b) == 1, "");
        qy::CloseChineseState(b);
        Check("关闭后句柄与视图都已清空", b.view == nullptr && b.map == nullptr, "");
        qy::ChineseState c = qy::OpenChineseStateNamed(kName);
        Check("关掉一个实例后新实例仍读到同一真值", c.view && qy::SharedChinese(c) == 1, "");
        qy::CloseChineseState(c);
        qy::CloseChineseState(a);
    }
    qy::ChineseState empty;
    Check("映射不可用时读返回 0（降级不崩）", qy::SharedChinese(empty) == 0, "");
    qy::SetSharedChinese(empty, true);
    Check("映射不可用时写也安全", true, "");

    // ---- 6. 跨进程可见（真实场景：输入法 DLL 跑在多个宿主进程里）----
    {
        qy::ChineseState parent =
            qy::OpenChineseStateNamed(L"Local\\QiuYuWubiCN_xproc_v1");
        if (parent.view) ::InterlockedExchange(&parent.view->chinese, 0);
        char self[MAX_PATH] = {};
        ::GetModuleFileNameA(nullptr, self, MAX_PATH);
        std::string cmd = std::string("\"") + self + "\" --set 2";
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        const BOOL ok = ::CreateProcessA(nullptr, (LPSTR)cmd.c_str(), nullptr, nullptr, FALSE,
                                         0, nullptr, nullptr, &si, &pi);
        if (ok) {
            ::WaitForSingleObject(pi.hProcess, 5000);
            ::CloseHandle(pi.hThread);
            ::CloseHandle(pi.hProcess);
        }
        Check("跨进程：另一个进程写英文→本进程读到 2",
              ok && parent.view && qy::SharedChinese(parent) == 2, "");
        qy::CloseChineseState(parent);
    }

    printf("FAILED=%d\n", gFail);
    if (gOut) {
        fprintf(gOut, "FAILED=%d\n", gFail);
        fclose(gOut);
    }
    return gFail ? 1 : 0;
}
