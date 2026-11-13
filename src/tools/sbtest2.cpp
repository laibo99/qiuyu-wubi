// 状态条交互自测：窗口位置、点击分区、右键菜单回调
#include <cstdio>
#include <windows.h>
#include "../core/qy_statusbar.h"

static int gRegion = -1;
static int gMenu = 0;
static int gFail = 0;

static void Check(const char* name, bool ok, const char* detail) {
    fprintf(stdout, "[%s] %s %s\n", ok ? "PASS" : "FAIL", name, detail);
    fflush(stdout);
    if (!ok) ++gFail;
}

int main() {
    HINSTANCE h = ::GetModuleHandleW(nullptr);
    qy::StatusBar sb;
    if (!sb.Create(h)) {
        printf("Create failed\n");
        return 2;
    }
    sb.SetClickHandler([](int r) { gRegion = r; });
    sb.SetMenuHandler([](POINT) { ++gMenu; });

    qy::StatusInfo si;
    si.chinese = true;
    si.mode = qy::Mode::Wubi;
    sb.Update(si);
    sb.Show();

    RECT wr;
    ::GetWindowRect(sb.hwnd(), &wr);
    char buf[512];
    sprintf(buf, "窗口 %ldx%ld @(%ld,%ld)", wr.right - wr.left, wr.bottom - wr.top, wr.left,
            wr.top);
    // 状态条按 DPI/皮肤缩放（设计尺寸 140x28），点击坐标按实际窗口等比换算
    const long W = wr.right - wr.left, H = wr.bottom - wr.top;
    auto X = [&](long x) {
        return MAKELPARAM((WORD)(x * W / 140), (WORD)(14 * H / 28));
    };
    Check("状态条出现在屏幕内", ::IsWindowVisible(sb.hwnd()) && wr.left >= 0 && wr.top >= 0 &&
                                     W > 0, buf);

    // 左半区点击 -> 区域 0（切中英）
    gRegion = -1;
    ::SendMessageW(sb.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, X(8));
    ::SendMessageW(sb.hwnd(), WM_LBUTTONUP, 0, X(8));
    sprintf(buf, "region=%d", gRegion);
    Check("点左侧=切中英", gRegion == 0, buf);

    // 右半区点击 -> 区域 1（切五笔/拼音）
    gRegion = -1;
    ::SendMessageW(sb.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, X(52));
    ::SendMessageW(sb.hwnd(), WM_LBUTTONUP, 0, X(52));
    sprintf(buf, "region=%d", gRegion);
    Check("点「五/拼」=切方案", gRegion == 1, buf);

    // 全/半
    gRegion = -1;
    ::SendMessageW(sb.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, X(72));
    ::SendMessageW(sb.hwnd(), WM_LBUTTONUP, 0, X(72));
    sprintf(buf, "region=%d", gRegion);
    Check("点「全/半」=切全角", gRegion == 2, buf);

    // 小键盘
    gRegion = -1;
    ::SendMessageW(sb.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, X(99));
    ::SendMessageW(sb.hwnd(), WM_LBUTTONUP, 0, X(99));
    sprintf(buf, "region=%d", gRegion);
    Check("点小键盘图标", gRegion == 3, buf);

    // 设置
    gRegion = -1;
    ::SendMessageW(sb.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, X(126));
    ::SendMessageW(sb.hwnd(), WM_LBUTTONUP, 0, X(126));
    sprintf(buf, "region=%d", gRegion);
    Check("点设置图标", gRegion == 4, buf);

    // 拖动（位移大）不应触发切换，而应触发位置回调。
    // 拖动是“相对当前窗口位置”跟随（客户区绝对式会形成反馈回路，见
    // qy_statusbar.cpp 的 WM_MOUSEMOVE 注释），所以 SendMessage 模拟即可：
    // MOVE 事件里窗口会被挪动，UP 用同样比例的坐标确认没有误触发点击。
    int moved = 0;
    sb.SetMoveHandler([&](int, int) { ++moved; });
    gRegion = -1;
    ::SendMessageW(sb.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, X(10));
    ::SendMessageW(sb.hwnd(), WM_MOUSEMOVE, MK_LBUTTON, X(90));
    ::SendMessageW(sb.hwnd(), WM_LBUTTONUP, 0, X(90));
    sprintf(buf, "moved=%d region=%d", moved, gRegion);
    Check("拖动不误触发切换", moved == 1 && gRegion == -1, buf);

    // 拖出超过阈值后又拖回原点：dragActive_ 一旦置位就不回退，
    // 松手仍然只算拖动，不能触发点击（防“想拖结果手一抖切了开关”）
    gRegion = -1;
    ::SendMessageW(sb.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, X(40));
    ::SendMessageW(sb.hwnd(), WM_MOUSEMOVE, MK_LBUTTON, X(120));
    ::SendMessageW(sb.hwnd(), WM_MOUSEMOVE, MK_LBUTTON, X(40));
    ::SendMessageW(sb.hwnd(), WM_LBUTTONUP, 0, X(40));
    sprintf(buf, "region=%d", gRegion);
    Check("拖回原点也不触发点击", gRegion == -1, buf);

    // 阈值内（约 3 物理像素）的轻移仍算点击，功能不受影响
    gRegion = -1;
    ::SendMessageW(sb.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, X(126));
    ::SendMessageW(sb.hwnd(), WM_MOUSEMOVE, MK_LBUTTON, X(128));
    ::SendMessageW(sb.hwnd(), WM_LBUTTONUP, 0, X(128));
    sprintf(buf, "region=%d", gRegion);
    Check("3px 内轻移仍算点击", gRegion == 4, buf);

    // 慢速拖动：每次只挪 1 设计像素，且按“光标屏幕坐标”推进（真实慢拖就是这样：
    // 窗口跟着走，客户区坐标几乎不变）。老实现用客户区坐标减按下点，位移会被
    // 窗口跟随抵消、永远攒不到阈值，松手就被当成单击 —— 用户看到的
    // “慢速拖动还是会触发点击功能，快速拖动才正常”。
    {
        RECT w0;
        ::GetWindowRect(sb.hwnd(), &w0);
        int cx = w0.left + (int)(60 * W / 140);  // 光标屏幕坐标
        int cy = w0.top + (int)(14 * H / 28);
        const int grabX = cx - w0.left, grabY = cy - w0.top;
        const int startLeft = w0.left;
        moved = 0;
        gRegion = -1;
        ::SendMessageW(sb.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(grabX, grabY));
        for (int k = 1; k <= 20; ++k) {
            cx += (int)(1 * W / 140);
            RECT w;
            ::GetWindowRect(sb.hwnd(), &w);
            ::SendMessageW(sb.hwnd(), WM_MOUSEMOVE, MK_LBUTTON,
                           MAKELPARAM(cx - w.left, cy - w.top));
        }
        RECT w;
        ::GetWindowRect(sb.hwnd(), &w);
        ::SendMessageW(sb.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(cx - w.left, cy - w.top));
        sprintf(buf, "moved=%d region=%d dx=%ld", moved, gRegion,
                (long)(w.left - startLeft));
        Check("慢速拖动不触发点击", moved == 1 && gRegion == -1, buf);
        Check("慢速拖动窗口跟得上", ::GetWindowRect(sb.hwnd(), &w) &&
                                        (w.left - startLeft) >= (int)(15 * W / 140), buf);
    }

    // 右键 -> 菜单回调
    ::SendMessageW(sb.hwnd(), WM_RBUTTONUP, 0, X(40));
    sprintf(buf, "menu=%d", gMenu);
    Check("右键弹出菜单", gMenu == 1, buf);

    // ---- GHO 精简 Win10 场景回归：无桌面合成时状态条必须恒为实体 ----
    // 根因：老实现在“鼠标不在条上”时把整体 alpha 设成 160（半透明），鼠标进出
    // 就在 160/255 之间反复整窗提交；GHO 这类关掉 Themes/DWM 的系统合成不稳，
    // 表现为画面乱跳 + 桌面残影。修复后：compositionOk=false 时无论鼠标在哪、
    // 是否拖动，alpha 一律 255（不闪烁）。同时不能把正常系统的“鼠标在外才半透明”
    // 行为改坏。
    {
        int a;
        a = qy::StatusBar::ComputeLayeredAlpha(false, true, false, false);
        Check("GHO 无合成+半透明+鼠标在外→实体255", a == 255, "");
        a = qy::StatusBar::ComputeLayeredAlpha(false, true, true, false);
        Check("GHO 无合成+半透明+鼠标在内→实体255", a == 255, "");
        a = qy::StatusBar::ComputeLayeredAlpha(false, true, false, true);
        Check("GHO 无合成+半透明+拖动中→实体255", a == 255, "");
        a = qy::StatusBar::ComputeLayeredAlpha(false, false, false, false);
        Check("GHO 无合成+本就不透明→实体255", a == 255, "");
        // 正常系统（合成可用）仍保留“鼠标在外才半透明”，反向确认没改坏
        a = qy::StatusBar::ComputeLayeredAlpha(true, true, false, false);
        Check("合成可用+半透明+鼠标在外→半透明160", a == 160, "");
        a = qy::StatusBar::ComputeLayeredAlpha(true, true, true, false);
        Check("合成可用+鼠标在内→实体255", a == 255, "");
        a = qy::StatusBar::ComputeLayeredAlpha(true, true, false, true);
        Check("合成可用+拖动中→实体255", a == 255, "");
        a = qy::StatusBar::ComputeLayeredAlpha(true, false, false, false);
        Check("合成可用+本就不透明→实体255", a == 255, "");
    }

    // ---- “鼠标在条上”恒实体：单一闪烁的核心防护 ----
    // 单一闪烁的根因是 mouseInside_ 被外部事件（如打开设置造成的前台切换，
    // 系统给无焦点状态条发虚假 WM_MOUSELEAVE）误清成 false，使 alpha 在
    // 160/255 之间跳。去抖逻辑保证收到 LEAVE 不会立即降 alpha；只要
    // mouseInside 保持 true，无论 compos/drag/translucent 怎么变，alpha 都
    // 必须是 255，绝不因任何外部状态跳变。这里穷举确认这条契约。
    {
        int bad = 0;
        for (int comp = 0; comp <= 1; ++comp)
            for (int trans = 0; trans <= 1; ++trans)
                for (int drag = 0; drag <= 1; ++drag) {
                    int a = qy::StatusBar::ComputeLayeredAlpha((bool)comp, (bool)trans,
                                                              true, (bool)drag);
                    if (a != 255) ++bad;
                }
        Check("鼠标在条上时所有组合都恒为实体255（单一闪烁防护）", bad == 0, "");
    }

    // ---- Win10 状态条四角「直角灰角标」回归 ----
    // 根因：窗口类带了 CS_DROPSHADOW，系统给 WS_EX_LAYERED 窗口生成的阴影按
    // **窗口矩形**算，完全不理会 UpdateLayeredWindow 的 per-pixel alpha 圆角，
    // 于是胶囊右/下各留一条硬边、四角各留一块直角灰方块。修复后不再有系统
    // 阴影，卡片仍靠渐变 + 1px 圆角描边 + 顶部内高光保持层次。
    {
        const DWORD cls = ::GetClassLongW(sb.hwnd(), GCL_STYLE);
        Check("窗口类不带 CS_DROPSHADOW（四角无直角灰块）",
              (cls & (DWORD)CS_DROPSHADOW) == 0, "");
        const LONG ex = ::GetWindowLongW(sb.hwnd(), GWL_EXSTYLE);
        Check("仍是分层窗口（圆角靠 per-pixel alpha）", (ex & WS_EX_LAYERED) != 0, "");
    }

    sb.Hide();
    Check("可隐藏", !::IsWindowVisible(sb.hwnd()), "");
    sb.Destroy();

    printf("FAILED=%d\n", gFail);
    return gFail ? 1 : 0;
}
