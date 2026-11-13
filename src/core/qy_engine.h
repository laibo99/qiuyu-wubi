// 秋雨五笔输入法 - 输入引擎
#pragma once
#include "qy_common.h"
#include "qy_config.h"
#include "qy_dict.h"
#include <unordered_map>

namespace qy {

// 三个并列的输入方案：纯五笔 / 纯拼音 / 五笔拼音混输
enum class Mode { Wubi = 0, Pinyin = 1, Mixed = 2 };

inline int ModeToInt(Mode m) { return (int)m; }
inline Mode ModeFromInt(int v) {
    return v == 1 ? Mode::Pinyin : (v == 2 ? Mode::Mixed : Mode::Wubi);
}

struct Candidate {
    std::wstring text;
    std::string code;   // 显示用：五笔码 / 拼音
    uint32_t weight = 0;
    uint32_t learned = 0;
    uint32_t boost = 0; // 手动调频：>0 表示用户按 Ctrl+数字 置顶过，值越大越靠前
    int rank = 0;       // 0=精确匹配 1=前缀匹配
};

class Engine {
public:
    bool Init(const std::wstring& dataDir);
    bool ready() const { return dict_.loaded(); }

    void SetOptions(const Options& o) { opt_ = o; if (buf_.empty()) return; Update(); }
    Options& options() { return opt_; }
    const Options& options() const { return opt_; }

    Mode mode() const { return mode_; }
    void SetMode(Mode m) { if (m != mode_) { mode_ = m; Clear(); } }
    // 方案轮换：五笔 → 拼音 → 混输 → 五笔
    void ToggleMode() {
        SetMode(mode_ == Mode::Wubi ? Mode::Pinyin
                                    : (mode_ == Mode::Pinyin ? Mode::Mixed : Mode::Wubi));
    }

    // 临时拼音：五笔方案下按 z 进入的**一次性**拼音输入状态。
    // 只改变本次组合的候选查询，绝不改动 mode_（用户方案与 config.ini 仍是五笔）；
    // 上屏 / 回车清屏 / Esc / 切方案 / 宿主终止组合时由 Clear() 自动复位。
    // z 是纯引导键，不进入编码缓冲：进入后接着打全拼即可；想拼 z 开头的音节
    // （zhong / zai）就再按一次 z，此时它已是普通拼音字母。
    bool CanTempPinyin() const;   // 仅「五笔 + 编码为空 + 尚未进入」时成立
    void BeginTempPinyin();       // 进入临时拼音
    void CancelTempPinyin();      // 放弃本次临时拼音，回到五笔
    bool tempPinyin() const { return tempPinyin_; }

    bool hasComp() const { return !buf_.empty(); }
    const std::string& buffer() const { return buf_; }
    const std::wstring& bufferW() const { return bufW_; }

    // 输入动作，返回是否已消费
    bool OnLetter(char c);
    bool OnDigit(int d);
    bool OnSpace();
    bool OnEnter();
    bool OnBackspace();
    bool OnEscape();
    bool OnPageKey(bool next);

    // 全角转换（受“全角/半角”开关控制）
    std::wstring ToWidth(const std::wstring& s) const;

    bool TakeCommit(std::wstring& out);
    void QueueCommit(const std::wstring& s) { pending_ += s; }
    // TSF 编辑会话失败后重试时用：判断提交是否还在队列里，避免补队列时把符号插两次
    bool HasPendingCommit() const { return !pending_.empty(); }
    void Clear();
    void Reload();

    const std::vector<Candidate>& cands() const { return cands_; }
    // 本次候选里来自“五笔码”的数量（混输时总候选数会更多，四码唯一上屏要用它判断）
    size_t wubiCount() const { return wubiCount_; }
    // 本次混输里拼音匹配到的条数（0 = 这串字母打不出拼音）
    size_t pinyinHits() const { return pinyinHits_; }
    size_t pageNo() const { return page_; }
    size_t pageCount() const;
    size_t pageStart() const { return page_ * (size_t)opt_.pageSize; }
    bool NextPage();
    bool PrevPage();
    bool SelectOnPage(size_t idxInPage);
    bool SelectGlobal(size_t idx);

    void Learn(const std::wstring& text);
    void SaveUser();
    void LoadUser();

    // 手动调频：把当前页第 idxInPage 个候选调到第一位（Ctrl+数字），记录会持久化
    bool PromoteOnPage(size_t idxInPage);
    bool PromoteGlobal(size_t idx);
    // 清除某个编码下的调频记录；code 为空则清空全部
    void ClearPromotions(const std::string& code = std::string());
    size_t promoCount() const { return promo_.size(); }
    void SavePromote();
    void LoadPromote();

    // 中文标点转换，返回空串表示原样输出
    std::wstring punct(wchar_t ascii, bool shift);

    Dict& dict() { return dict_; }

private:
    void Update();
    void CommitIndex(size_t i);
    // 这串字母能不能当拼音打出来（混输里判断“第五码该上屏还是继续拼音”）
    bool PinyinHasMatch(const std::string& s) const;

    static bool IsGbkText(const std::wstring& s);

    Dict dict_;
    Options opt_;
    Mode mode_ = Mode::Wubi;
    bool tempPinyin_ = false;  // 一次性临时拼音（五笔方案下 z 引导）
    std::string buf_;
    std::wstring bufW_;
    std::vector<Candidate> cands_;
    size_t wubiCount_ = 0;
    size_t wubiExact_ = 0;    // 四码精确命中的五笔候选数（唯一上屏只认它）
    size_t pinyinHits_ = 0;   // 混输时拼音命中的条数（0 表示这串字母不能当拼音）
    bool pinyinExtendable_ = false;  // 这串字母后面还能接着打拼音（shiy → shiyan）
    size_t page_ = 0;
    std::wstring pending_;
    std::unordered_map<std::wstring, uint32_t> learn_;
    // 手动调频表：key = 编码 + '\x01' + 候选词，value = 置顶序号（越大越靠前）
    std::unordered_map<std::wstring, uint32_t> promo_;
    uint32_t promoSeq_ = 0;
    bool promoDirty_ = false;
    bool userDirty_ = false;
    bool quoteOpen_ = false;   // 双引号配对
    bool squoteOpen_ = false;  // 单引号配对
    std::wstring dataDir_;
};

}  // namespace qy
