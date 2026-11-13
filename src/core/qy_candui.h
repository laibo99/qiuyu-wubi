// 秋雨五笔输入法 - 候选窗口
#pragma once
#include "qy_common.h"
#include "qy_engine.h"
#include <functional>

namespace qy {

struct CandShow {
    const std::vector<Candidate>* items = nullptr;
    size_t page = 0;
    size_t pageSize = 5;
    POINT pt{0, 0};      // 屏幕坐标基准点（光标左下角）
    int fontSize = 20;
    bool showCode = true;
    bool horizontal = true;
    bool showTip = true;
    int sel = 0;         // 页内高亮序号
    int total = 0;       // 候选总数
};

class CandWindow {
public:
    bool Create(HINSTANCE hInst);
    void Destroy();
    void Hide();
    bool visible() const { return visible_; }
    void Show(const CandShow& p);
    void SetClickHandler(std::function<void(int)> fn) { onClick_ = std::move(fn); }
    int HitTest(POINT screenPt) const;
    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void OnPaint();
    bool IsDarkMode() const;
    // 前台窗口不属于本进程（用户切到别的程序）时自动隐藏，
    // 否则候选窗为 TOPMOST 弹窗，会一直浮在屏幕上不消失
    void CheckForeground();

    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr;
    HFONT fontSmall_ = nullptr;
    bool visible_ = false;
    CandShow data_;
    std::vector<Candidate> items_;
    std::vector<RECT> itemRects_;
    size_t pageCount_ = 1;
    int fontSize_ = 20;
    bool dark_ = false;
    std::function<void(int)> onClick_;
};

}  // namespace qy
