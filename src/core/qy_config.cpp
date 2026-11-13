// 秋雨五笔输入法 - 配置实现
#include "qy_config.h"

namespace qy {

std::wstring Config::Path() const { return UserDir() + L"\\config.ini"; }

static int ReadInt(const wchar_t* sec, const wchar_t* key, int def, const std::wstring& path) {
    return (int)::GetPrivateProfileIntW(sec, key, def, path.c_str());
}

void Config::Load() {
    std::wstring p = Path();
    if (::GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    opt_.mode = ReadInt(L"方案", L"默认", opt_.mode, p);
    opt_.pageSize = ReadInt(L"界面", L"候选个数", opt_.pageSize, p);
    opt_.fontSize = ReadInt(L"界面", L"字号", opt_.fontSize, p);
    opt_.layout = ReadInt(L"界面", L"竖排", opt_.layout, p);
    opt_.showCode = ReadInt(L"界面", L"显示编码", opt_.showCode, p);
    opt_.showCandTip = ReadInt(L"界面", L"显示提示", opt_.showCandTip, p);
    opt_.autoUnique4 = ReadInt(L"输入", L"四码唯一自动上屏", opt_.autoUnique4, p);
    opt_.autoNext5 = ReadInt(L"输入", L"五码自动上屏", opt_.autoNext5, p);
    opt_.freqAdjust = ReadInt(L"输入", L"词频调整", opt_.freqAdjust, p);
    opt_.startChinese = ReadInt(L"输入", L"新窗口默认中文", opt_.startChinese, p);
    opt_.smartPunct = ReadInt(L"输入", L"中文标点", opt_.smartPunct, p);
    opt_.fuzzyPinyin = ReadInt(L"输入", L"模糊音", opt_.fuzzyPinyin, p);
    opt_.gbkOnly = ReadInt(L"编码", L"仅GBK", opt_.gbkOnly, p);
    opt_.statusBar = ReadInt(L"界面", L"状态条", opt_.statusBar, p);
    opt_.sbTheme = ReadInt(L"界面", L"状态条主题", opt_.sbTheme, p);
    opt_.sbSkin = ReadInt(L"界面", L"状态条皮肤", opt_.sbSkin, p);
    opt_.sbTranslucent = ReadInt(L"界面", L"状态条半透明", opt_.sbTranslucent, p);
    opt_.sbAccent = ReadInt(L"界面", L"状态条强调色", opt_.sbAccent, p);
    opt_.sbCard = ReadInt(L"界面", L"状态条底色", opt_.sbCard, p);
    opt_.fullWidth = ReadInt(L"界面", L"全角", opt_.fullWidth, p);
    opt_.numpadSelect = ReadInt(L"界面", L"小键盘选词", opt_.numpadSelect, p);
    opt_.shiftEnglish = ReadInt(L"界面", L"Shift英文", opt_.shiftEnglish, p);
    opt_.shiftToggle = ReadInt(L"界面", L"Shift切换中英", opt_.shiftToggle, p);
    opt_.ctrlSpace = ReadInt(L"界面", L"Ctrl空格", opt_.ctrlSpace, p);
    opt_.manualFreq = ReadInt(L"输入", L"手动调频", opt_.manualFreq, p);
    opt_.handwriting = ReadInt(L"输入", L"手写输入", opt_.handwriting, p);
    opt_.statusX = ReadInt(L"界面", L"状态条X", opt_.statusX, p);
    opt_.statusY = ReadInt(L"界面", L"状态条Y", opt_.statusY, p);
    // 更新地址：配置里没写或写空 → 用内置默认地址（改了默认值对老用户也生效）
    {
        wchar_t buf[1024] = {0};
        ::GetPrivateProfileStringW(L"更新", L"地址", L"", buf, 1024, p.c_str());
        opt_.updateUrl = buf[0] ? std::wstring(buf) : std::wstring(QY_DEFAULT_UPDATE_URL);
    }
    opt_.autoCheckUpdate = ReadInt(L"更新", L"自动检测", opt_.autoCheckUpdate, p);
    {
        wchar_t buf[64] = {0};
        ::GetPrivateProfileStringW(L"更新", L"上次检测", L"0", buf, 64, p.c_str());
        opt_.lastUpdateCheck = _wtoi64(buf);
    }
    if (opt_.pageSize < 1) opt_.pageSize = 1;
    if (opt_.pageSize > 9) opt_.pageSize = 9;
    if (opt_.fontSize < 12) opt_.fontSize = 12;
    if (opt_.fontSize > 48) opt_.fontSize = 48;
    if (opt_.mode < 0 || opt_.mode > 2) opt_.mode = 0;  // 0 五笔 / 1 拼音 / 2 混输
    // 老配置里可能是空值或脏值：皮肤只认 0/1/2，越界一律退回默认皮肤
    if (opt_.sbSkin < 0 || opt_.sbSkin > 2) opt_.sbSkin = 0;
}

void Config::Save() const {
    std::wstring p = Path();
    auto wi = [&](const wchar_t* sec, const wchar_t* key, int v) {
        wchar_t buf[32];
        wsprintfW(buf, L"%d", v);
        ::WritePrivateProfileStringW(sec, key, buf, p.c_str());
    };
    wi(L"方案", L"默认", opt_.mode);
    wi(L"界面", L"候选个数", opt_.pageSize);
    wi(L"界面", L"字号", opt_.fontSize);
    wi(L"界面", L"竖排", opt_.layout);
    wi(L"界面", L"显示编码", opt_.showCode);
    wi(L"界面", L"显示提示", opt_.showCandTip);
    wi(L"输入", L"四码唯一自动上屏", opt_.autoUnique4);
    wi(L"输入", L"五码自动上屏", opt_.autoNext5);
    wi(L"输入", L"词频调整", opt_.freqAdjust);
    wi(L"输入", L"新窗口默认中文", opt_.startChinese);
    wi(L"输入", L"中文标点", opt_.smartPunct);
    wi(L"输入", L"模糊音", opt_.fuzzyPinyin);
    wi(L"编码", L"仅GBK", opt_.gbkOnly);
    wi(L"界面", L"状态条", opt_.statusBar);
    wi(L"界面", L"状态条主题", opt_.sbTheme);
    wi(L"界面", L"状态条皮肤", opt_.sbSkin);
    wi(L"界面", L"状态条半透明", opt_.sbTranslucent);
    wi(L"界面", L"状态条强调色", opt_.sbAccent);
    wi(L"界面", L"状态条底色", opt_.sbCard);
    wi(L"界面", L"全角", opt_.fullWidth);
    wi(L"界面", L"小键盘选词", opt_.numpadSelect);
    wi(L"界面", L"Shift英文", opt_.shiftEnglish);
    wi(L"界面", L"Shift切换中英", opt_.shiftToggle);
    wi(L"界面", L"Ctrl空格", opt_.ctrlSpace);
    wi(L"输入", L"手动调频", opt_.manualFreq);
    wi(L"输入", L"手写输入", opt_.handwriting);
    wi(L"界面", L"状态条X", opt_.statusX);
    wi(L"界面", L"状态条Y", opt_.statusY);
    // 与内置默认一致时写空：以后改默认地址，老用户也能自动跟上
    ::WritePrivateProfileStringW(
        L"更新", L"地址",
        (opt_.updateUrl == QY_DEFAULT_UPDATE_URL) ? L"" : opt_.updateUrl.c_str(), p.c_str());
    wi(L"更新", L"自动检测", opt_.autoCheckUpdate);
    {
        // 注意：wsprintfW 不支持 %lld（会原样写出“ld”），这里手工转十进制
        long long v = opt_.lastUpdateCheck;
        wchar_t buf[32];
        int i = 31;
        buf[i] = 0;
        bool neg = v < 0;
        unsigned long long u = neg ? (unsigned long long)(-(v + 1)) + 1ULL : (unsigned long long)v;
        if (u == 0) buf[--i] = L'0';
        while (u > 0) {
            buf[--i] = (wchar_t)(L'0' + (int)(u % 10));
            u /= 10;
        }
        if (neg) buf[--i] = L'-';
        ::WritePrivateProfileStringW(L"更新", L"上次检测", buf + i, p.c_str());
    }
}

}  // namespace qy
