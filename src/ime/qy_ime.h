// 秋雨五笔输入法 - TSF 文本服务
#pragma once
#include <msctf.h>
#include <string>
#include <vector>
#include "../core/qy_engine.h"
#include "../core/qy_candui.h"
#include "../core/qy_statusbar.h"
#include "../core/qy_symui.h"

#ifndef TF_INVALID_GUIDATOM
#define TF_INVALID_GUIDATOM ((TfGuidAtom)0)
#endif

namespace qy {

// {7E2F1C4A-9B3D-4A6E-8F52-1C3D5A7B9E01}
static const CLSID CLSID_QYTextService = {0x7e2f1c4a, 0x9b3d, 0x4a6e,
                                          {0x8f, 0x52, 0x1c, 0x3d, 0x5a, 0x7b, 0x9e, 0x01}};
// {7E2F1C4A-9B3D-4A6E-8F52-1C3D5A7B9E02} 语言配置（五笔）
static const GUID GUID_QYProfileWubi = {0x7e2f1c4a, 0x9b3d, 0x4a6e,
                                        {0x8f, 0x52, 0x1c, 0x3d, 0x5a, 0x7b, 0x9e, 0x02}};
// {7E2F1C4A-9B3D-4A6E-8F52-1C3D5A7B9E03} 语言配置（拼音）
static const GUID GUID_QYProfilePinyin = {0x7e2f1c4a, 0x9b3d, 0x4a6e,
                                          {0x8f, 0x52, 0x1c, 0x3d, 0x5a, 0x7b, 0x9e, 0x03}};
// {7E2F1C4A-9B3D-4A6E-8F52-1C3D5A7B9E04} 显示属性
static const GUID GUID_QYDisplayAttrInput = {0x7e2f1c4a, 0x9b3d, 0x4a6e,
                                             {0x8f, 0x52, 0x1c, 0x3d, 0x5a, 0x7b, 0x9e, 0x04}};
// {7E2F1C4A-9B3D-4A6E-8F52-1C3D5A7B9E05} 保留键：Ctrl+空格
static const GUID GUID_QYKeyCtrlSpace = {0x7e2f1c4a, 0x9b3d, 0x4a6e,
                                         {0x8f, 0x52, 0x1c, 0x3d, 0x5a, 0x7b, 0x9e, 0x05}};

constexpr UINT kTaskUpdate = 1;

class CTextService;

//------------------------------------------------------------------------------
// 编辑会话
class CEditSession : public ITfEditSession {
public:
    CEditSession(CTextService* svc, ITfContext* ctx);
    ~CEditSession();

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;
    STDMETHODIMP DoEditSession(TfEditCookie ec) override;

private:
    LONG _cRef;
    CTextService* _svc;
    ITfContext* _ctx;
};

//------------------------------------------------------------------------------
// 文本服务
class CTextService : public ITfTextInputProcessor,
                     public ITfThreadMgrEventSink,
                     public ITfKeyEventSink,
                     public ITfCompositionSink,
                     public ITfCompartmentEventSink {
public:
    CTextService();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // ITfTextInputProcessor
    STDMETHODIMP Activate(ITfThreadMgr* ptim, TfClientId tid) override;
    STDMETHODIMP Deactivate() override;

    // ITfThreadMgrEventSink
    STDMETHODIMP OnInitDocumentMgr(ITfDocumentMgr* pdim) override;
    STDMETHODIMP OnUninitDocumentMgr(ITfDocumentMgr* pdim) override;
    STDMETHODIMP OnSetFocus(ITfDocumentMgr* pdimFocus, ITfDocumentMgr* pdimPrevFocus) override;
    STDMETHODIMP OnPushContext(ITfContext* pic) override;
    STDMETHODIMP OnPopContext(ITfContext* pic) override;

    // ITfKeyEventSink
    STDMETHODIMP OnSetFocus(BOOL fForeground) override;
    STDMETHODIMP OnTestKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam,
                               BOOL* pfEaten) override;
    STDMETHODIMP OnTestKeyUp(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnKeyUp(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnPreservedKey(ITfContext* pic, REFGUID rguid, BOOL* pfEaten) override;

    // ITfCompositionSink
    STDMETHODIMP OnCompositionTerminated(TfEditCookie ecWrite, ITfComposition* pComposition) override;

    // ITfCompartmentEventSink
    // Ctrl+空格 是系统级热键（Input Method\Hot Keys），按下时不会进按键回调，
    // 系统只是切换 GUID_COMPARTMENT_KEYBOARD_OPENCLOSE 隔间。
    // 监听它，才能让 Ctrl+空格 真正切换中/英。
    STDMETHODIMP OnChange(REFGUID rguid) override;

    // 供编辑会话调用
    HRESULT Apply(TfEditCookie ec, ITfContext* pic);

private:
    ~CTextService();

    HRESULT _Activate(ITfThreadMgr* ptim, TfClientId tid);
    BOOL _InitKeyEventSink();
    void _UninitKeyEventSink();
    // 用 TSF 保留键抢下 Ctrl+空格（系统热键会先吞掉这个组合）
    void _SyncPreservedKey();
    BOOL _InitThreadMgrEventSink();
    void _UninitThreadMgrEventSink();
    // 中/英 与系统“输入法开/关”状态同步（Ctrl+空格 走这里）
    BOOL _InitCompartmentSink();
    void _UninitCompartmentSink();
    bool _ReadKeyboardOpen(int* out) const;
    void _WriteKeyboardOpen(bool open);
    void _SetChinese(bool chinese, const wchar_t* why = nullptr);
    void _AdoptKeyboardState();

    HRESULT _UpdateComposition(TfEditCookie ec, ITfContext* pic);
    HRESULT _EndComposition(TfEditCookie ec, ITfContext* pic);
    // 取消组合：清空组合范围内的编码字母后结束，用于回车清屏 / Esc，防止字母残留到文档
    HRESULT _CancelComposition(TfEditCookie ec, ITfContext* pic);
    HRESULT _SetAttribute(TfEditCookie ec, ITfContext* pic, ITfRange* pRange, bool clear);
    HRESULT _Sync(ITfContext* pic);
    void _ShowCandidate(ITfContext* pic, TfEditCookie ec);
    void _HideCandidate();
    void _CommitNow(const std::wstring& text);

    bool _TestEaten(WPARAM vk, LPARAM lp);
    wchar_t _VkToChar(WPARAM vk, bool shift) const;
    void _LoadConfig();

    // 状态条与方案切换
    void _UpdateStatus();
    void _ToggleMode();
    void _ToggleChinese();
    void _ToggleWidth();
    void _ToggleNumpad();
    void _SaveOption(int which);
    void _SetMode(Mode m);
    void _ShowMenu(POINT pt);
    void _OpenSettings();
    void _ToggleSettings();  // 打开/关闭设置窗口（再次点击齿轮 = 关闭）
    // 图标输入（符号面板）
    void _ToggleSymbols();
    void _HideSymbols();
    void _InsertSymbol(const std::wstring& s);
    void _SendUnicode(const std::wstring& s);
    // 取“当前焦点”的上下文（GetFocus → GetTop）。宿主失焦时可能拿到已断开的
    // 上下文，返回值仍非空，交给调用方用 TF_E_DISCONNECTED 判断
    ITfContext* _AcquireFocusContext();
    // 把符号按 TSF 标准流程提交；上下文断开时先 SetFocus 把文档焦点接回来再重试
    HRESULT _CommitSymbolTsf(const std::wstring& s, bool hadComp, bool* restored);
    void _SaveModeToDisk();
    void _CheckConfigReload();
    void _SyncChineseFromShared();  // 250ms tick：静默跟上跨实例唯一的中/英真值
    void _PollWin7Shift();          // Win7 专用：250ms 轮询 Shift 边沿，轻按切中/英
    void _SyncStatusVisible();
    // 新版本检测：manual=true 由用户主动发起（会提示结果/错误），
    // false 为启动时的自动检测（静默失败，发现新版才弹确认框）
    void _CheckUpdate(bool manual);
    // 打开增量更新程序（qyupd.exe）：只下载有变动的文件
    bool _LaunchUpdater();

    LONG _cRef;
    ITfThreadMgr* _pThreadMgr = nullptr;
    TfClientId _tid = 0;
    DWORD _dwThreadMgrSinkCookie = TF_INVALID_COOKIE;
    ITfComposition* _pComposition = nullptr;
    ITfContext* _pLastContext = nullptr;
    // 最近一次拿到焦点的 document manager（AddRef 持有）。MS Office 在点到自己
    // 面板窗口之类的事件里会把 TSF 焦点丢掉，上下文随即变成“已断开”，此时
    // SetFocus 回它就能把同一个文档的上下文重新接上，符号才插得进去
    ITfDocumentMgr* _pDocMgr = nullptr;
    ITfCategoryMgr* _pCategoryMgr = nullptr;
    ITfCompartment* _pOpenClose = nullptr;      // GUID_COMPARTMENT_KEYBOARD_OPENCLOSE
    DWORD _dwCompSinkCookie = TF_INVALID_COOKIE;
    bool _compGuard = false;                    // 自己写隔间时屏蔽回环通知
    bool _endingComposition = false;
    bool _cancelCompBeforeCommit = false;  // 插入符号前先抹掉未上屏的编码字母
    HWND _symTarget = nullptr;             // 打开符号面板时的前台窗口（输出目标）
    bool _chinese = true;
    bool _chineseInited = false;  // 中/英是否已确定（首次激活后置位，之后不再被系统值覆盖）
    DWORD _activateTick = 0;      // 最近一次激活时刻：隔间被系统重置的宽限期起点
    // 按键时间戳在 OnTestKeyDown 里记（那个回调对**所有** keydown 都会来，
    // 不像 OnKeyDown 只收「被吃」的键）。以前记在 OnKeyDown 里，而 VK_CONTROL
    // 被 _TestEaten 判为不吃 → 永远到不了 → keyJustPressed 恒 false，Win10 上
    // 真按 Ctrl+空格 也被当噪声忽略。
    DWORD _modKeyTick = 0;         // 最近一次按下 Ctrl
    DWORD _ctrlSpaceTick = 0;      // 最近一次看到 Ctrl+空格 组合
    DWORD _lastCompFollowTick = 0;  // 上次跟随隔间切换的时刻（GHO 上防高频乒乓）
    DWORD _lastCompLogTick = 0;     // OnChange 日志限流（事件风暴时别刷爆 trace.log）
    bool _preservedKeyOk = false;   // Ctrl+空格 保留键是否注册成功（排障用）
    ChineseState _sharedState;  // 跨实例唯一的中/英真值（句柄+视图，见 qy_common.h）
    bool _shiftDown = false;
    DWORD _shiftPollDownTick = 0;   // Win7 专用：轮询检测到的 Shift 按下时刻（0=当前未按）
    bool _shiftPollUsed = false;    // Win7 专用：Shift 按下期间是否被当作修饰键用（打大写/符号）
    bool _shiftUsedAsEnglish = false;  // 按住 Shift 打过英文字母，松开不再切换中英
    DWORD _shiftTick = 0;
    bool _foreground = true;

    Engine _engine;
    CandWindow _cand;
    StatusBar _status;
    SymbolPanel _sym;
    Options _opt;
    HMODULE _hInst = nullptr;
    int _sel = 0;
    std::wstring _dataDir;
    FILETIME _cfgTime{};
    DWORD _cfgCheckTick = 0;
};

}  // namespace qy
