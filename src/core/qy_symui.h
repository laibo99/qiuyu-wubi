// 秋雨五笔输入法 - 图标输入（符号面板）
// 点状态条上的笑脸图标弹出：常用 / 数学 / 序号 / 箭头 / 符号 / 颜文字
// 点任意符号即插入到当前光标处，面板不抢焦点（点完可以继续点下一个）
#pragma once
#include "qy_common.h"
#include <functional>

namespace qy {

class SymbolPanel {
public:
    bool Create(HINSTANCE hInst);
    void Destroy();

    bool visible() const { return visible_; }
    HWND hwnd() const { return hwnd_; }

    // anchor = 状态条左上角（屏幕坐标），面板显示在它正上方；已经显示则收起
    void Toggle(POINT anchor);
    void Show(POINT anchor);
    void Hide();

    // 面板的主人窗口（状态条）：点击它由状态条自己处理（开/关面板），
    // 低级鼠标钩子对它放行，避免“点图标关面板”被钩子先收起又立刻弹回
    void SetOwnerWindow(HWND h) { owner_ = h; }
    // 自检用：可替换“前台窗口”的来源（默认 GetForegroundWindow）
    void SetForegroundProbe(std::function<HWND()> fn) { fgProbe_ = std::move(fn); }
    // 自检用：沙箱里注入的真实鼠标事件到不了本进程，直接把屏幕坐标喂给
    // 低级钩子回调，验证“点外面收起 / 点主人窗口放行”的判定
    static void DebugInjectMouse(UINT msg, POINT pt) {
        MSLLHOOKSTRUCT hs{};
        hs.pt = pt;
        MouseHook(0, msg, (LPARAM)&hs);
    }

    // 选中某个符号（插入文本）
    void SetPickHandler(std::function<void(const std::wstring&)> fn) { onPick_ = std::move(fn); }
    // 显示/隐藏状态变化（用于让状态条图标高亮）
    void SetVisibleHandler(std::function<void()> fn) { onVisible_ = std::move(fn); }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void OnPaint(HDC dc);
    void CheckForeground();
    void ApplySize();
    int RowsOf(int cat) const;
    int ColsOf(int cat) const;
    int PanelH() const;
    // 返回 item 序号；isTab=true 时返回 tab 序号
    int HitTest(POINT clientPt, bool* isTab) const;
    static LRESULT CALLBACK MouseHook(int code, WPARAM w, LPARAM l);
    void InstallHook();
    void RemoveHook();

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;      // 状态条窗口（钩子白名单）
    HHOOK hook_ = nullptr;      // WH_MOUSE_LL：面板显示期间精确接收“点外面”
    std::function<HWND()> fgProbe_;
    bool visible_ = false;
    bool dark_ = false;
    int cat_ = 0;
    int hoverItem_ = -1;
    int hoverTab_ = -1;
    int flashItem_ = -1;    // 刚插入的格子（短暂高亮，给个“点到了”的反馈）
    int pendingIdx_ = -1;   // 待插入符号的格子序号
    POINT anchor_{0, 0};
    DWORD shownTick_ = 0;
    std::wstring pending_;  // 点了但还没输出的符号（延后到鼠标消息之外再插入）
    HFONT fontTab_ = nullptr;
    HFONT fontIcon_ = nullptr;
    HFONT fontIconSmall_ = nullptr;
    std::function<void(const std::wstring&)> onPick_;
    std::function<void()> onVisible_;
};

}  // namespace qy
