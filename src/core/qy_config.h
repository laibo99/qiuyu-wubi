// 秋雨五笔输入法 - 配置
#pragma once
#include "qy_common.h"
#include "qy_version.h"

namespace qy {

struct Options {
    int mode = 0;              // 启动方案：0=五笔 1=拼音 2=五笔拼音混输
    int pageSize = 5;          // 候选个数
    int fontSize = 20;         // 候选字号
    int layout = 0;            // 0=横排 1=竖排
    int autoUnique4 = 1;       // 四码唯一自动上屏
    int autoNext5 = 1;         // 五码自动上屏首选
    int freqAdjust = 1;        // 词频调整
    int startChinese = 1;      // 新窗口/新程序里默认中文输入（否则跟随系统隔间的英文状态）
    int smartPunct = 1;        // 中文标点
    int showCode = 1;          // 候选显示编码
    int gbkOnly = 1;           // 仅输出 GBK 字符集内汉字
    int fuzzyPinyin = 0;       // 拼音模糊音（z=zh c=ch s=sh n=l）
    int showCandTip = 1;       // 显示翻页提示
    int statusBar = 1;         // 显示输入法状态条
    int sbTheme = 0;           // 状态条主题：0=跟随系统 1=浅色 2=深色
    int sbSkin = 0;            // 状态条皮肤：0=默认（圆角+阴影） 1=经典（直角+黑细描边+无阴影）
                               // 2=彩框（圆角胶囊+彩色线条+无阴影，线色随中英/配色变化）
    int sbTranslucent = 1;     // 状态条半透明：平时半透明，鼠标靠近恢复实体
    int sbAccent = 0;          // 状态条强调色（COLORREF，0=默认蓝），DIY 配色
    int sbCard = 0;            // 状态条底色（COLORREF，0=默认），DIY 图样
    int fullWidth = 0;         // 全角输出（0=半角 1=全角）
    int numpadSelect = 1;      // 小键盘数字用于选候选（0=直接输入数字）
    int shiftEnglish = 1;      // 按住 Shift 直接输入英文
    int shiftToggle = 1;       // 单击 Shift 切换中/英文
    int ctrlSpace = 1;         // Ctrl+空格 切换中英文
    int manualFreq = 1;        // Ctrl+数字 手动调频（把第 N 个候选调到第一位）
    int handwriting = 0;       // 手写输入：调用系统自带手写面板（Win10/11 触摸键盘 TabTip）
    int statusX = -1;          // 状态条位置（-1 表示默认右下角）
    int statusY = -1;
    std::wstring theme = L"默认";  // 候选皮肤（预留）

    // ---- 新版本检测 ----
    // 更新信息地址（version.txt；http/https 或本地/共享路径），为空则不自动检测
    std::wstring updateUrl = QY_DEFAULT_UPDATE_URL;
    int autoCheckUpdate = 1;         // 启动时自动检测新版本（每 12 小时最多一次）
    long long lastUpdateCheck = 0;   // 上次自动检测的时间（time_t），0 表示从未检测
};

class Config {
public:
    void Load();
    void Save() const;
    std::wstring Path() const;
    Options& opt() { return opt_; }
    const Options& opt() const { return opt_; }

private:
    Options opt_;
};

}  // namespace qy
