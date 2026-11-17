// 本轮三项修复的回归护栏（2026-11-06）
//
//  ① 回车清屏全局失效：_CancelComposition 必须「先 SetText 删掉完整组合范围、
//     再 Collapse + SetSelection」。以前先 Collapse 成空插入点再 SetText(L"")，
//     一个字符都没删，结束组合后编码字母留在文档里 → 所有宿主都失效。
//  ② 状态条三款皮肤：配置往返 / 越界回退 / 端到端接线（右键菜单 → 渲染 → 设置菜单），
//     含彩框皮肤（shape=2）的线色随中英状态 / 强调色变化。
//  ③ 设置面板菜单化：六个主菜单、关于子菜单里的版本与官方主页、勾选走唯一映射。
//
// 组合了「真跑」与「源码接线检查」两类断言：配置与 IsClassicSkin 是真调用，
// 其余是轻量源码护栏，防止以后重构时把某一段接线悄悄删掉。
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <windows.h>
#include "../core/qy_common.h"
#include "../core/qy_config.h"
#include "../core/qy_statusbar.h"

static int gFail = 0;
static FILE* gOut = nullptr;

static void Check(const char* name, bool ok, const char* detail = "") {
    printf("[%s] %s %s\n", ok ? "PASS" : "FAIL", name, detail);
    fflush(stdout);
    if (gOut) fprintf(gOut, "[%s] %s %s\n", ok ? "PASS" : "FAIL", name, detail);
    if (!ok) ++gFail;
}

static std::string ReadFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::string();
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool Has(const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
}

int main() {
    gOut = fopen("build/fixtest.txt", "w");
    const std::string ime = ReadFile("src/ime/qy_ime.cpp");
    const std::string sb = ReadFile("src/core/qy_statusbar.cpp");
    const std::string demo = ReadFile("src/tools/qydemo.cpp");
    const std::string cfg = ReadFile("src/core/qy_config.cpp");
    const std::string ver = ReadFile("src/core/qy_version.h");
    const std::string sbh = ReadFile("src/core/qy_statusbar.h");
    const std::string upd = ReadFile("src/tools/qyupd.cpp");
    const std::string updc = ReadFile("src/core/qy_update.cpp");
    const std::string engh = ReadFile("src/core/qy_engine.h");
    const std::string eng = ReadFile("src/core/qy_engine.cpp");
    const std::string setup = ReadFile("src/tools/qysetup.cpp");
    const std::string bsh = ReadFile("build.sh");
    const std::string minc = ReadFile("tools/make_inc.py");
    Check("源码可读", !ime.empty() && !sb.empty() && !demo.empty() && !cfg.empty() &&
                          !ver.empty() && !sbh.empty() && !upd.empty() && !updc.empty() &&
                          !engh.empty() && !eng.empty() && !setup.empty() && !bsh.empty() &&
                          !minc.empty());

    // ---- ① 回车清屏：调用顺序是这条修复的全部，顺序反了就全局失效 ----
    const size_t body = ime.find("HRESULT CTextService::_CancelComposition");
    const size_t setText = ime.find("pRange->SetText(ec, 0, L\"\", 0)", body);
    const size_t collapse = ime.find("pRange->Collapse(ec, TF_ANCHOR_END)", body);
    const size_t sel = ime.find("pic->SetSelection(ec, 1, &sel)", body);
    Check("回车清屏：_CancelComposition 存在", body != std::string::npos);
    Check("回车清屏：SetText 用完整组合范围删除", setText != std::string::npos);
    Check("回车清屏：SetText 在 Collapse 之前",
          setText != std::string::npos && collapse != std::string::npos && setText < collapse);
    Check("回车清屏：删完仍显式 SetSelection（保住光标不左移）",
          collapse != std::string::npos && sel != std::string::npos && collapse < sel);

    // ---- ② 皮肤配置：写盘→读回、越界一律退回默认皮肤 ----
    wchar_t tmp[MAX_PATH] = {0};
    ::GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"qywb_fixtest";
    ::CreateDirectoryW(dir.c_str(), nullptr);
    ::SetEnvironmentVariableW(L"QYWB_USERDIR", dir.c_str());
    const std::wstring ini = dir + L"\\config.ini";
    ::DeleteFileW(ini.c_str());
    {
        qy::Config c;
        c.opt().sbSkin = 1;
        c.Save();
        qy::Config d;
        d.Load();
        Check("皮肤配置：经典=1 写盘读回", d.opt().sbSkin == 1);

        c.opt().sbSkin = 2;
        c.Save();
        qy::Config c2;
        c2.Load();
        Check("皮肤配置：彩框=2 写盘读回", c2.opt().sbSkin == 2);

        c.opt().sbSkin = 0;
        c.Save();
        qy::Config e;
        e.Load();
        Check("皮肤配置：默认=0 写盘读回", e.opt().sbSkin == 0);

        c.opt().sbSkin = 7;
        c.Save();
        qy::Config f;
        f.Load();
        Check("皮肤配置：越界值退回默认皮肤", f.opt().sbSkin == 0);
    }
    Check("配置文件有 [界面] 状态条皮肤 读写",
          Has(cfg, "L\"状态条皮肤\"") && Has(cfg, "opt_.sbSkin < 0 || opt_.sbSkin > 2"));
    ::DeleteFileW(ini.c_str());

    Check("IsClassicSkin：0 默认 / 1 经典 / 其余非经典",
          !qy::IsClassicSkin(0) && qy::IsClassicSkin(1) && !qy::IsClassicSkin(2) &&
              !qy::IsClassicSkin(-1));
    Check("IsOutlineSkin / IsShadowlessSkin 三款形状判定",
          !qy::IsOutlineSkin(0) && !qy::IsOutlineSkin(1) && qy::IsOutlineSkin(2) &&
              !qy::IsShadowlessSkin(0) && qy::IsShadowlessSkin(1) &&
              qy::IsShadowlessSkin(2) && !qy::IsShadowlessSkin(7));
    // 彩框线色：中文态取彩色（随强调色变化），英文态固定中性灰
    const COLORREF blue = RGB(40, 92, 206), red = RGB(220, 40, 40);
    const COLORREF gray = RGB(140, 147, 160);
    Check("彩框线色：中文彩色 / 英文灰色 / 随配色变化",
          qy::SkinFrameColor(true, blue, gray) == blue &&
              qy::SkinFrameColor(false, blue, gray) == gray &&
              qy::SkinFrameColor(true, red, gray) == red &&
              qy::SkinFrameColor(false, red, gray) == gray);
    qy::StatusSkin def;
    Check("默认皮肤结构未变（圆角+阴影那套）",
          def.shape == 0 && def.translucent == 1 && def.accent == 0 && def.card == 0);

    // ---- ② 端到端接线：渲染 / 输入法右键菜单 / 设置面板 ----
    Check("渲染：经典分支关圆角、纯色平涂；默认皮肤仍保留阴影",
          Has(sb, "const bool classic = IsClassicSkin(skin_.shape);") &&
              Has(sb, "const float cR = classic ? 0.0f") &&
              Has(sb, "IsShadowlessSkin(skin_.shape) ? 0 : 64") &&
              Has(sb, "FillRectFlat(sdc, 0, 0, bw, bh, pal.cardTop);"));
    Check("渲染：经典四周黑色细描边",
          Has(sb, "p.border = RGB(0, 0, 0);") &&
              Has(sb, "StrokeRectFlat(sdc, 0, 0, bw, bh, S, pal.border)"));
    Check("渲染：彩框沿用圆角渐变胶囊 + 彩色线框 + 不画内高光",
          Has(sb, "const bool outline = IsOutlineSkin(skin_.shape);") &&
              Has(sb, "SkinFrameColor(info_.chinese, pal.pill2, pal.pill2Off)") &&
              Has(sb, "2 * S, frameCol") && Has(sb, "if (!classic && !outline)") &&
              !Has(sb, "const int SHADOW_MAX = classic ? 0 : 64;"));
    Check("输入法：皮肤读到状态条、三款右键切换落盘",
          Has(ime, "s.shape = o.sbSkin;") && Has(ime, "if (which == 7) o.sbSkin = _opt.sbSkin;") &&
              Has(ime, "L\"状态条皮肤\"") && Has(ime, "case 21:") && Has(ime, "case 22:") &&
              Has(ime, "case 23:") && Has(ime, "L\"彩框皮肤") &&
              Has(ime, "_SaveOption(7);") && Has(ime, "ApplyStatusSkin(_status, _opt);"));
    Check("设置面板：三款皮肤互斥勾选 + 命令分支",
          Has(demo, "ID_MENU_SKIN_COLOR") && Has(demo, "L\"皮肤：彩框") &&
              Has(demo, "case ID_MENU_SKIN_COLOR:") &&
              Has(demo, "o.sbSkin == 2 ? MF_CHECKED : MF_UNCHECKED"));

    // ---- ③ 设置面板菜单化 ----
    const wchar_t* menus[] = {L"编辑(&E)", L"选择(&S)", L"窗口(&W)",
                              L"功能(&F)", L"帮助(&H)", L"关于(&A)"};
    // 注意：上面是宽字符字面量，需要转成 UTF-8 才能在源码文本里查找
    auto w2u8 = [](const wchar_t* w) {
        char b[128] = {0};
        ::WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof(b), nullptr, nullptr);
        return std::string(b);
    };
    bool allMenus = true;
    for (const wchar_t* m : menus) allMenus = allMenus && Has(demo, w2u8(m).c_str());
    Check("设置面板：编辑/选择/窗口/功能/帮助/关于 六个主菜单", allMenus);
    Check("设置面板：菜单已挂载 + 展开时刷新状态",
          Has(demo, "BuildMainMenu(h);") && Has(demo, "case WM_INITMENUPOPUP:") &&
              Has(demo, "::DestroyMenu(g_menuBar);"));
    Check("关于：官方主页与发布页面两个网址分别可打开、可显示",
          Has(demo, "ID_MENU_ABOUT_VER") && Has(demo, "ID_MENU_ABOUT_HOME") &&
              Has(demo, "ID_MENU_ABOUT_RELEASE") &&
              Has(demo, "OpenUrl(QY_HOME_URL)") && Has(demo, "OpenUrl(QY_SITE_URL)") &&
              Has(demo, "官方主页：") && Has(demo, "五笔输入法发布页面（下载）：") &&
              Has(ver, "#define QY_HOME_URL L\"http://sam.gd.cn\"") &&
              Has(ver, "#define QY_SITE_URL L\"http://down.sam.gd.cn/downloads/update\"") &&
              Has(ime, "QY_HOME_URL") && Has(ime, "QY_SITE_URL"));
    Check("选择：15 个勾选项走 OptionSlot 唯一映射",
          Has(demo, "static const int kChkCount = 15;") &&
              Has(demo, "OptionSlot(g_cfg.opt(), id - ID_CHK_BASE)") &&
              Has(demo, "CommitOptions(h);"));
    Check("版本自绘已从窗口右上角移除",
          !Has(demo, "右上角版本号：和安装记录同一来源"));

    // ---- ④ 本轮修正：使用说明弹窗 / 功能菜单去项 ----
    Check("帮助：查看使用说明改为弹出独立内容窗口",
          Has(demo, "ShowHelpPopup(h);") && Has(demo, "kHelpPopupClass") &&
              Has(demo, "::RegisterClassExW(&wc3);") &&
              Has(demo, "HelpPopupProc") && Has(demo, "g_helpPopup"));
    Check("帮助：说明全文两种模式都缓存 + 多路径读取",
          Has(demo, "g_helpText = help;") && Has(demo, "DecodeHelpBytes") &&
              Has(demo, "LoadHelpFile()"));
    Check("功能：已去掉「切换方案：五笔/拼音/混输」",
          !Has(demo, "切换方案：五笔") && !Has(demo, "case ID_BTN_PINYIN"));

    // ---- ⑤ 部分程序里状态条自动消失 ----
    Check("状态条：隐藏前检查目标进程是否有实例接管",
          Has(sb, "StatusBarExistsInProcess(pid)") &&
              Has(sb, "AnyStatusVisibleInThisProc()") &&
              Has(sb, "fgCanTakeOver") && Has(sb, "imeStillActive"));
    Check("状态条：TSF 活跃心跳由真实输入活动（按键/焦点/开启）注入",
          Has(sbh, "void NoteImeActivity()") && Has(sbh, "imeActiveTick_") &&
              Has(ime, "if (_foreground) _status.NoteImeActivity();") &&
              Has(ime, "_status.NoteImeActivity();"));
    Check("状态条：自检工具不注入心跳时仍走既有收起规则",
          Has(sb, "imeActiveTick_ != 0") && Has(sb, "kImeActiveGraceMs"));

    // ---- ⑥ 切换窗口时状态条没有消失 ----
    // 根因：tick 每 250ms 按 _foreground 无条件续期心跳；_foreground 默认
    // true 且切窗口未必收到 OnSetFocus(FALSE) → 心跳永不过期 → 条常驻。
    const size_t tick = ime.find("_status.SetTickHandler");
    const size_t tickEnd = ime.find("});", tick);
    const std::string tickBody =
        (tick != std::string::npos && tickEnd != std::string::npos)
            ? ime.substr(tick, tickEnd - tick)
            : std::string();
    Check("切窗口不消失：tick 里不再无条件续期活跃心跳",
          !tickBody.empty() && !Has(tickBody, "NoteImeActivity"));
    Check("切窗口不消失：用前台焦点控件 PID 即时判定真假切走",
          Has(sb, "ForegroundFocusPid") && Has(sb, "GetGUIThreadInfo(0") &&
              Has(sb, "focusPidProbe_") &&
              Has(sb, "focusIsMine || (focusPid == 0 && imeStillActive)") &&
              Has(sbh, "void SetFocusPidProbe"));
    Check("切窗口不消失：UWP 壳窗口子链复核，不误伤上一轮修复",
          Has(sb, "WindowHasChildInMyProcess(fg)") && Has(sb, "EnumChildWindows"));
    Check("切窗口不消失：心跳只作「拿不到焦点 PID」时的兜底",
          Has(sb, "focusPid == 0 && imeStillActive") && Has(sb, "kImeActiveGraceMs"));

    // ---- ⑦ 4.5.3 追加：窗口缩窄 / 切回默认中文 / 竖排候选右键 ----
    Check("设置窗口：宽度由 900 横向缩小 1/4 到 675，子控件动态布局",
          Has(demo, "kWindowW = 675") && Has(demo, "kClientW = 659") &&
              Has(demo, "LayoutMain(h)") && Has(demo, "g_helpCard.right = w - 16") &&
              !Has(demo, "900, 720") && !Has(demo, "852, 300"));
    Check("竖排候选：右键菜单有勾选项，写回 [界面] 竖排",
          Has(ime, "L\"竖排候选\"") && Has(ime, "flags(_opt.layout != 0), 24") &&
              Has(ime, "case 24:") && Has(ime, "if (which == 8) o.layout = _opt.layout;") &&
              Has(ime, "_SaveOption(8);"));
    const size_t deact = ime.find("STDMETHODIMP CTextService::Deactivate()");
    const std::string deactBody =
        deact != std::string::npos ? ime.substr(deact, 1800) : std::string();
    Check("英文关闭：Deactivate 无条件把跨实例真值复位为中文",
          Has(deactBody, "deactivate: force shared chinese") &&
              Has(deactBody, "SetSharedChinese(_sharedState, true)") &&
              Has(deactBody, "CloseChineseState(_sharedState);") &&
              !Has(deactBody, "_opt.startChinese"));
    const size_t compInit = ime.find("BOOL CTextService::_InitCompartmentSink()");
    const size_t compEnd = ime.find("void CTextService::_UninitCompartmentSink()", compInit);
    const std::string compBody =
        (compInit != std::string::npos && compEnd != std::string::npos)
            ? ime.substr(compInit, compEnd - compInit)
            : std::string();
    Check("切回来：激活与前台回来两条路径都强制中文，OnChange 仍不写回隔间",
          Has(compBody, "_SetChinese(true, L\"activate-force\")") &&
              Has(compBody, "_SyncChineseFromShared();") &&
              Has(ime, "if (_foreground && !wasForeground)") &&
              Has(ime, "_SetChinese(true, L\"foreground-return\")") &&
              Has(ime, "writeBack=0"));

    // ---- ⑧ 增量更新防降级：本地 4.5.3 / 服务器 4.5.2 不该列出 6 个待更新文件 ----
    Check("防降级：qy_update 提供独立判定，按逐段版本比较而非字符串",
          Has(updc, "bool IncUpdateWouldDowngrade") &&
              Has(updc, "CompareVersion(localVer, *v) > 0") &&
              Has(updc, "if (!localTrusted || localVer.empty()) return false"));
    Check("防降级：DoCheck 在比对 CRC 之前清空变更并给出原因",
          Has(upd, "IncUpdateWouldDowngrade(g_curVer, info.version, m.version, g_localVerTrusted)") &&
              Has(upd, "g_blockReason =") && Has(upd, "g_changes.clear()") &&
              Has(upd, "g_localVerTrusted = !g_curVer.empty()"));
    Check("防降级：界面显示原因、开始更新禁用、写入线程有第二道闸",
          Has(upd, "if (!g_blockReason.empty())") &&
              Has(upd, "g_blockReason.empty() && !g_changes.empty()") &&
              Has(upd, "!checkOnly && !g_blockReason.empty()"));
    Check("防降级：写回 installed.ver / 注册表不写比本地旧的版本号",
          Has(upd, "CompareVersion(g_curVer, g_newVer) > 0") &&
              Has(upd, "WriteInstalledVer(finalVer)") &&
              Has(upd, "UpdateRegistryVersion(finalVer)"));
    Check("防降级：检查更新以 installed.ver 为真值，不用编译期旧号",
          Has(ime, "std::wstring cur = DiskVersion();") &&
              Has(demo, "qy::CompareVersion(info.version, ShownVersion()) > 0"));

    // ---- ⑧b 同版本不同构建（2026-11-08 事故：本机 15:05 的 4.5.3 遇到
    //       服务器 10:22 的 4.5.3 清单，仍被列出 3 个待更新文件）----
    Check("防构建倒退：qy_update 提供构建戳比较与同版本判定",
          Has(updc, "int CompareBuildStamp(") && Has(updc, "bool IncUpdateIsStaleBuild(") &&
              Has(updc, "if (CompareVersion(localVer, manifestVer) != 0) return false;") &&
              Has(updc, "return CompareBuildStamp(localBuild, manifestBuild) > 0;"));
    Check("防构建倒退：清单解析 build= 行、installed.build 读写齐全",
          Has(updc, "m.build = val;") && Has(updc, "std::wstring InstalledBuildOnDisk(") &&
              Has(updc, "bool WriteInstalledBuild("));
    Check("防构建倒退：qyupd 在 CRC 比对前用构建戳拦，且提示写明两个时间",
          Has(upd, "IncUpdateIsStaleBuild(g_curVer, m.version, g_localBuild, g_serverBuild") &&
              Has(upd, "g_serverBuild = m.build.empty() ? manifestTime : m.build;") &&
              Has(upd, "BuildStampText(g_localBuild)") &&
              Has(upd, "BuildStampFromInstalledVerMtime(dir)"));
    Check("防构建倒退：更新成功后把清单构建戳写回 installed.build",
          Has(upd, "if (!g_serverBuild.empty()) WriteInstalledBuild(g_installDir, g_serverBuild);"));
    Check("防构建倒退：安装程序写 installed.build 并在卸载时清理",
          Has(setup, "QY_BUILD_STAMP") && Has(setup, "installed.build"));
    Check("防构建倒退：构建脚本注入唯一构建戳，清单与二进制同源",
          Has(bsh, "BUILD_STAMP=\"${QY_BUILD_STAMP:-$(date +%Y%m%d%H%M%S)}\"") &&
              Has(bsh, "export QY_BUILD_STAMP") && Has(bsh, "-DQY_BUILD_STAMP=L\\\"$BUILD_STAMP\\\"") &&
              Has(minc, "f'build={build_stamp}'") &&
              Has(ver, "#ifndef QY_BUILD_STAMP"));

    // ---- ⑨ 临时拼音功能键 z（2026-11-08） ----
    Check("临时拼音：引擎有一次性状态与引导 / 取消接口",
          Has(engh, "bool CanTempPinyin() const") && Has(engh, "void BeginTempPinyin()") &&
              Has(engh, "void CancelTempPinyin()") && Has(engh, "bool tempPinyin_ = false"));
    Check("临时拼音：只在「五笔 + 空编码 + 尚未进入」时把 z 当功能键",
          Has(eng, "mode_ == Mode::Wubi && !tempPinyin_ && buf_.empty()"));
    Check("临时拼音：候选查询按有效方案走拼音，四码唯一上屏绕开",
          Has(eng, "const Mode mode = tempPinyin_ ? Mode::Pinyin : mode_") &&
              Has(eng, "const bool pyLike = (mode_ == Mode::Pinyin || tempPinyin_)") &&
              Has(eng, "if (!pyLike && buf_.size() == 4 && opt_.autoUnique4)"));
    Check("临时拼音：Clear 统一复位（上屏 / 回车 / Esc / 切方案都汇到这里）",
          Has(eng, "tempPinyin_ = false"));
    Check("临时拼音：五笔空编码按 z 引导，z 本身不进编码缓冲",
          Has(ime, "wParam == 'Z' && _engine.CanTempPinyin()") &&
              Has(ime, "_engine.BeginTempPinyin()") &&
              Has(eng, "编码缓冲保持为空：z 不进编码"));
    Check("临时拼音：空组合时 Esc / 回车 / 退格也能退出",
          Has(ime, "_engine.tempPinyin() && !_engine.hasComp()") &&
              Has(ime, "_engine.CancelTempPinyin()") &&
              Has(ime, "QyTrace(L\"temp pinyin: cancel\")"));
    Check("临时拼音：状态条图标始终读真实 mode()，临时拼音期间不改成「拼」",
          Has(ime, "si.mode = _engine.mode();") &&
              !Has(ime, "_engine.tempPinyin() ? Mode::Pinyin : _engine.mode()") &&
              Has(ime, "cfg.opt().mode = ModeToInt(_engine.mode());"));
    Check("临时拼音：点状态条「拼」是退回五笔，不是把方案轮换掉",
          Has(ime, "void CTextService::_ToggleMode()") &&
              Has(ime, "if (_engine.tempPinyin()) {") && Has(ime, "_engine.ToggleMode();"));

    printf("FAILED=%d\n", gFail);
    if (gOut) {
        fprintf(gOut, "FAILED=%d\n", gFail);
        fclose(gOut);
    }
    return gFail ? 1 : 0;
}
