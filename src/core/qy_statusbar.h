// 秋雨五笔输入法 - 输入法状态条
// 浮动小窗：中/英 · 五/拼 · 全/半 · 图标输入 · 设置
// 点对应区域即可切换，右键出菜单，可拖动
// 绘制方式：逻辑坐标 → 4 倍超采样 → 缩小（HALFTONE），得到抗锯齿的圆角与图标
#pragma once
#include "qy_common.h"
#include "qy_engine.h"
#include <functional>

namespace qy {

struct StatusInfo {
    bool chinese = true;
    Mode mode = Mode::Wubi;
    bool fullWidth = false;   // 全角
    bool symbols = false;     // 图标输入（符号面板）是否打开
};

// 状态条外观（DIY）：由 config.ini 的 [界面] 段驱动
struct StatusSkin {
    int theme = 0;        // 0=跟随系统 1=浅色 2=深色
    int shape = 0;        // 0=默认皮肤（圆角+阴影） 1=经典（直角+黑细描边+无阴影）
                          // 2=彩框（默认胶囊造型 + 彩色线框 + 无阴影）
    int translucent = 1;  // 平时半透明，鼠标靠近恢复实体
    COLORREF accent = 0;  // 强调色（0=默认蓝）；中文字形 / 胶囊 / 笑脸都用它
    COLORREF card = 0;    // 底色（0=默认）
};

// 皮肤形状：集中在这里，渲染各处的圆角/阴影判断都走它，避免漏改。
inline bool IsClassicSkin(int shape) { return shape == 1; }
// 彩框皮肤：沿用默认皮肤的圆角胶囊，只是换成彩色线框、去掉阴影。
inline bool IsOutlineSkin(int shape) { return shape == 2; }
inline bool IsShadowlessSkin(int shape) {
    return IsClassicSkin(shape) || IsOutlineSkin(shape);
}
// 彩框线色与「中/英」按键颜色同源：中文态用彩色（胶囊暗端，由强调色派生，
// DIY 配色与浅深主题都自动跟随），英文态用灰胶囊暗端（中性灰）。
// 抽成纯函数便于自测直接穷举，不依赖 GDI / 窗口。
inline COLORREF SkinFrameColor(bool chinese, COLORREF pillCn, COLORREF pillEn) {
    return chinese ? pillCn : pillEn;
}

// 状态条分区
enum StatusCell {
    kCellChinese = 0,  // 中 / 英
    kCellMode = 1,     // 五 / 拼
    kCellWidth = 2,    // 全 / 半
    kCellSymbols = 3,  // 图标输入（符号面板）
    kCellSettings = 4  // 设置
};

class StatusBar {
public:
    bool Create(HINSTANCE hInst);
    void Destroy();
    void Show();
    void Hide();
    // 配置里的“显示状态条”开关：关闭后不再自动弹出/自动恢复
    void SetEnabled(bool en);
    // 自检用：可替换“前台窗口”的来源（默认 GetForegroundWindow）
    void SetForegroundProbe(std::function<HWND()> fn) { fgProbe_ = std::move(fn); }
    // 自检用：可替换“前台焦点控件所属进程”的来源（默认 GetGUIThreadInfo(0)）
    void SetFocusPidProbe(std::function<DWORD()> fn) { focusPidProbe_ = std::move(fn); }

    // TSF 活跃心跳：输入法仍认为自己是前台 / 正在处理按键时由 IME 侧调用。
    // 用来区分「前台窗口属于代理进程（UWP / TextInputHost / 多进程窗口）」和
    // 「用户真的切到了别的程序」——前者的前台 PID 与本实例不同，但本实例
    // 还在被使用，不能把状态条收起，否则没有实例接管，条会自动消失且再
    // 也回不来。自检工具不注入心跳（一直是 0），继续走原来的收起规则。
    void NoteImeActivity() { imeActiveTick_ = ::GetTickCount(); }
    bool visible() const { return visible_; }
    HWND hwnd() const { return hwnd_; }
    void Update(const StatusInfo& s);
    void SetSkin(const StatusSkin& s);
    void SetPos(int x, int y);
    int x() const { return x_; }
    int y() const { return y_; }

    // 左键点击：region 见 StatusCell
    void SetClickHandler(std::function<void(int)> fn) { onClick_ = std::move(fn); }
    // 右键：弹出菜单（参数为屏幕坐标）
    void SetMenuHandler(std::function<void(POINT)> fn) { onMenu_ = std::move(fn); }
    // 拖动结束：回报新位置
    void SetMoveHandler(std::function<void(int, int)> fn) { onMove_ = std::move(fn); }
    // 每个前台检查周期（250ms）回调一次：输入法用它做配置热加载，
    // 这样在设置窗口里改配色/开关，状态条不用等按键就能立刻变
    void SetTickHandler(std::function<void()> fn) { onTick_ = std::move(fn); }

    // 分层窗口的整体 alpha 决策（供 Render 使用，也供自测直接验证）。
    // GHO 精简 Win10 常关掉 Themes/DWM，桌面合成不稳：此时无论如何都返回 255
    // （实体），不让 alpha 随鼠标进出在 160/255 之间反复整窗提交（那是“乱跳”
    // 的根）。正常系统且开启半透明、鼠标不在条上、未拖动时才返回 160。
    static int ComputeLayeredAlpha(bool compositionOk, bool translucent,
                                   bool mouseInside, bool dragging);

    // 跨实例 / 跨进程统一位置。能读到配置（%APPDATA%）的宿主把条放在用户拖好的
    // 位置；读不到的（explorer / 桌面这类跨完整性级别宿主，GHO 精简系统常见）
    // 通过同一份 NULL DACL 共享内存拿到同一份，避免前台一切换就"上下跳动"。
    static void SetSharedPos(int x, int y);

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void Render();  // 超采样绘制 → 逐像素 alpha → UpdateLayeredWindow（圆角无锯齿）
    void Place();
    // 前台不属于本线程时自动隐藏，前台回来时自动恢复显示
    void CheckForeground();
    int HitRegion(POINT clientPt) const;
    int L(int logical) const;  // 逻辑像素 → 物理像素

    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    HFONT fontSmall_ = nullptr;
    bool visible_ = false;
    bool enabled_ = false;  // 配置上是否允许显示（statusBar 开关）
    bool dark_ = false;
    StatusSkin skin_;
    bool mouseInside_ = false;  // 鼠标在条上（半透明 ↔ 实体切换依据）
    int dpi_ = 96;
    StatusInfo info_;
    int x_ = -1, y_ = -1;
    bool dragging_ = false;
    bool dragActive_ = false;  // 已越过阈值、确认是拖动：本次按下不再触发点击
    int dragMoved_ = 0;        // 自按下起的最大位移平方（屏幕坐标，单调递增）
    POINT dragStart_{0, 0};   // 按下时的客户区坐标（抓取点）
    POINT pressOrigin_{0, 0};  // 按下时窗口左上角（屏幕坐标）
    POINT pressCursor_{0, 0};  // 按下时光标位置（屏幕坐标 = pressOrigin_ + dragStart_）
    int hoverCell_ = -1;   // 鼠标悬停的分区
    int pressCell_ = -1;   // 按下中的分区
    bool tracking_ = false;

    // ---- 重绘节流（2026-11-04：GHO Win10 上画面乱跳 / 桌面黑影） ----
    // 设置窗口前台时配置轮询是 150ms 一次，鼠标进出还要切半透明；若每次
    // InvalidateRect 都整窗超采样 + UpdateLayeredWindow，合成不稳的系统上
    // 就会出现闪烁和残影。外观没变就别再提交一帧。
    DWORD lastRenderKey_ = 0;
    COLORREF lastAccent_ = 0, lastCard_ = 0;
    int lastW_ = 0, lastH_ = 0;
    bool forceRender_ = true;       // Show / 尺寸 / 皮肤变化后强制出一帧
    bool compositionOk_ = true;     // 桌面合成可用（无合成时不做整体半透明）
    bool compositionChecked_ = false;
    bool sbLogged_ = false;         // 首次 Render 的诊断日志只打一次
    RECT dragOldRect_{0, 0, 0, 0}; // 拖动开始前的窗口矩形，结束后用来刷掉残影
    DWORD imeActiveTick_ = 0;      // 最近一次 TSF 活跃时间（0=从未注入）
    // 前台窗口子窗口树复核结果缓存：只在换前台或满 1 秒时重新枚举，
    // 免得 250ms 一拍把 Chrome / Office 那几百个子窗口全遍历一遍。
    HWND lastFgHwnd_ = nullptr;
    bool lastProxyChild_ = false;
    DWORD lastProxyTick_ = 0;
    std::function<HWND()> fgProbe_;
    std::function<DWORD()> focusPidProbe_;
    std::function<void(int)> onClick_;
    std::function<void(POINT)> onMenu_;
    std::function<void(int, int)> onMove_;
    std::function<void()> onTick_;

    // 跨实例统一的位置缓存（进程内静态）。首次显示时若本实例没有有效位置，
    // 从这里取；取不到再去跨进程共享内存；都没有才回落默认右下角。
    static int s_posX_, s_posY_;
    static bool s_posInit_;
};

}  // namespace qy
