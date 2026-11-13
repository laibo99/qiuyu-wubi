// 秋雨五笔输入法 - 输入引擎实现
#include "qy_engine.h"
#include <algorithm>
#include <cstdlib>

namespace qy {

// 手动调频的键：编码 + 分隔符 + 词条。
// 用编码做键，保证“把 lg 的第 2 个候选调到首位”只影响 lg 这一组候选。
static std::wstring PromoKey(const std::string& code, const std::wstring& text) {
    std::wstring k = Utf8ToW(code);
    k.push_back(L'\x01');
    k += text;
    return k;
}

bool Engine::IsGbkText(const std::wstring& s) {
    BOOL used = FALSE;
    char buf[8];
    for (wchar_t ch : s) {
        int n = ::WideCharToMultiByte(936, WC_NO_BEST_FIT_CHARS, &ch, 1, buf, sizeof(buf), nullptr, &used);
        if (n <= 0 || used) return false;
    }
    return true;
}

bool Engine::Init(const std::wstring& dataDir) {
    dataDir_ = dataDir;
    if (!dict_.Load(dataDir + L"\\qywb.dat")) return false;
    LoadUser();
    LoadPromote();
    return true;
}

void Engine::Reload() {
    bool had = hasComp();
    (void)had;
    Clear();
    LoadUser();
    LoadPromote();
}

void Engine::Clear() {
    buf_.clear();
    bufW_.clear();
    cands_.clear();
    page_ = 0;
    wubiExact_ = 0;
    pinyinHits_ = 0;
    pinyinExtendable_ = false;
    // 「输入结束」的所有路径（上屏 CommitIndex、回车清屏、Esc、切方案、
    // 宿主终止组合、Reload）最终都汇到这里，临时拼音就在这里统一复位。
    tempPinyin_ = false;
}

bool Engine::CanTempPinyin() const {
    // 只在纯五笔、编码为空、且还没进入临时拼音时把 z 当引导键。
    // 已进入后 z 必须是普通拼音字母（zhong / zai 靠第二个 z 起头），
    // 否则用户永远打不出 z 开头的音节。
    return mode_ == Mode::Wubi && !tempPinyin_ && buf_.empty();
}

void Engine::BeginTempPinyin() {
    if (!CanTempPinyin()) return;
    tempPinyin_ = true;   // 编码缓冲保持为空：z 不进编码
}

void Engine::CancelTempPinyin() {
    if (!tempPinyin_) return;
    Clear();              // 顺手丢掉可能残留的半截拼音，回到五笔初始态
}

bool Engine::TakeCommit(std::wstring& out) {
    if (pending_.empty()) return false;
    out.swap(pending_);
    pending_.clear();
    return true;
}

size_t Engine::pageCount() const {
    size_t ps = (size_t)opt_.pageSize;
    if (ps == 0) ps = 5;
    return (cands_.size() + ps - 1) / ps;
}

bool Engine::NextPage() {
    if (page_ + 1 >= pageCount()) return false;
    ++page_;
    return true;
}

bool Engine::PrevPage() {
    if (page_ == 0) return false;
    --page_;
    return true;
}

void Engine::CommitIndex(size_t i) {
    if (i >= cands_.size()) return;
    const std::wstring& t = cands_[i].text;
    pending_ += t;
    Learn(t);
    Clear();
}

bool Engine::SelectGlobal(size_t idx) {
    if (idx >= cands_.size()) return false;
    CommitIndex(idx);
    return true;
}

bool Engine::SelectOnPage(size_t idxInPage) {
    size_t idx = page_ * (size_t)opt_.pageSize + idxInPage;
    return SelectGlobal(idx);
}

bool Engine::OnLetter(char c) {
    if (c < 'a' || c > 'z') return false;
    // 五笔 / 拼音 / 混输是三个并列方案：切到拼音就必须能打字母，
    // 纯五笔则完全按老规则走，不掺任何拼音候选。
    // 临时拼音按「纯拼音」对待：不受五笔四码长度限制、不触发四码唯一上屏。
    const bool pyLike = (mode_ == Mode::Pinyin || tempPinyin_);

    if (!pyLike && buf_.size() >= 4) {
        // 只有混输方案：再加一个字母还能当拼音打，就让位给拼音继续拼；
        // 打不出拼音才回到“第五码首选上屏后重新开始”的老行为
        bool keepPinyin = false;
        if (mode_ == Mode::Mixed && buf_.size() < 20) {
            std::string probe = buf_;
            probe.push_back(c);
            keepPinyin = PinyinHasMatch(probe);
        }
        if (!keepPinyin) {
            if (opt_.autoNext5 && !cands_.empty()) {
                CommitIndex(0);
                buf_.assign(1, c);
                bufW_.assign(1, (wchar_t)c);
                Update();
                return true;
            }
            return true;  // 超长 / 打不出拼音：忽略
        }
    }
    if (buf_.size() >= 24) return true;

    buf_.push_back(c);
    bufW_.push_back((wchar_t)c);
    Update();

    // 四码唯一自动上屏：必须是“只有一个五笔候选，而且这一条是四码精确命中”。
    // 混输方案还要保证这串字母不能继续当拼音打——否则打 shiy（想打 shiyan 试验）
    // 会被唯一的一条“五笔前缀”候选抢着上屏，拼音断了，看着就像混输没实现。
    if (!pyLike && buf_.size() == 4 && opt_.autoUnique4) {
        bool unique = (wubiCount_ == 1 && wubiExact_ == 1);
        if (unique && mode_ == Mode::Mixed && pinyinExtendable_) unique = false;
        if (unique) CommitIndex(0);
    }
    return true;
}

bool Engine::OnDigit(int d) {
    if (!hasComp()) return false;
    if (d < 1 || d > 9) return false;
    size_t idx = (size_t)(d - 1);
    if (idx >= cands_.size()) return false;
    if (page_ * (size_t)opt_.pageSize + idx >= cands_.size()) return false;
    return SelectOnPage(idx);
}

bool Engine::OnSpace() {
    if (!hasComp()) return false;
    if (cands_.empty()) { Clear(); return true; }
    CommitIndex(page_ * (size_t)opt_.pageSize);
    return true;
}

bool Engine::OnEnter() {
    // 回车清屏：清掉编码、候选以及尚未上屏的待提交内容，一个字符都不输出
    if (!hasComp()) return false;
    Clear();
    pending_.clear();
    quoteOpen_ = false;
    squoteOpen_ = false;
    return true;
}

bool Engine::OnBackspace() {
    if (!hasComp()) return false;
    buf_.pop_back();
    if (!bufW_.empty()) bufW_.pop_back();
    if (buf_.empty()) {
        cands_.clear();
        page_ = 0;
    } else {
        Update();
    }
    return true;
}

bool Engine::OnEscape() {
    if (!hasComp()) return false;
    Clear();
    return true;
}

bool Engine::OnPageKey(bool next) {
    if (!hasComp()) return false;
    return next ? NextPage() : PrevPage();
}

bool Engine::PinyinHasMatch(const std::string& s) const {
    if (s.empty()) return false;
    std::vector<uint32_t> tmp;
    dict_.SearchPinyin(s, tmp, 1);  // 只看有没有，取一条就够
    return !tmp.empty();
}

void Engine::Update() {
    // 临时拼音不改 mode_，只在这里把「有效方案」临时当成纯拼音。
    // 下面所有分支一律读 mode，别再直接读 mode_，否则候选会按五笔码去查。
    const Mode mode = tempPinyin_ ? Mode::Pinyin : mode_;
    cands_.clear();
    page_ = 0;
    wubiCount_ = 0;
    wubiExact_ = 0;
    pinyinHits_ = 0;
    pinyinExtendable_ = false;
    if (buf_.empty()) return;

    const bool mixed = (mode == Mode::Mixed);

    // 三个方案各自的名次分段（纯五笔 / 纯拼音都跟原来完全一致）：
    //   纯五笔：0 五笔精确 → 1 五笔前缀
    //   纯拼音：0 拼音精确 → 1 拼音前缀
    //   混输  ：0 精确（五笔精确 + 拼音精确，同档按词频比）
    //          → 1 五笔前缀 → 2 拼音前缀
    // 混输里拼音「精确」必须排在五笔「前缀」前面。否则打 ni / hao / wo 这种
    // 三两个字母时，五笔前缀能匹配几十上百条码，拼音候选全被挤到候选栏外面，
    // 用户看到的就是“混输没实现”。
    // 两三个字母时五笔只有简码能“精确”命中（如 ni→悄、wo→伙），跟拼音的
    // ni→你、wo→我 撞在一起：让词频决定先后，常用字优先。
    // 单字母例外：一级简码（w→人）必须压住，拼音一个字不成音节。
    const int kRankWubiExact = 0;
    const int kRankPyExact = mixed ? (buf_.size() >= 2 ? 0 : 1) : 0;
    const int kRankWubiPrefix = 1;
    const int kRankPyPrefix = mixed ? 2 : 1;

    // 混输时五笔前缀候选封顶（纯五笔不受影响）：单个字母能匹配几百条码，
    // 不封顶的话拼音前缀候选永远进不了 100 条候选的窗口。
    const size_t kWubiPrefixCap = 24;

    std::vector<uint32_t> hits;
    const bool wubiPhase = (mode != Mode::Pinyin) && buf_.size() <= 4;  // 五笔码最长 4 码
    if (mode != Mode::Pinyin) {
        if (wubiPhase) dict_.SearchWubi(buf_, hits, 100);
    } else {
        dict_.SearchPinyin(buf_, hits, 100);
    }

    std::vector<Candidate> tmp;
    tmp.reserve(128);
    std::unordered_map<std::wstring, size_t> pos;

    auto addHit = [&](uint32_t id, int rankExact, int rankPrefix, bool pinyinCode) {
        std::wstring text = dict_.Text(id);
        if (text.empty()) return;
        if (opt_.gbkOnly && !IsGbkText(text)) return;

        std::string code;
        if (pinyinCode) {
            std::string_view py = dict_.PyView(id);
            for (char ch : py)
                if (ch != '\'') code.push_back(ch);
        } else {
            code.assign(dict_.CodeView(id));
        }
        if (code.empty()) return;
        int rank = (code == buf_) ? rankExact : rankPrefix;

        auto it = pos.find(text);
        if (it != pos.end()) {
            Candidate& old = tmp[it->second];
            if (rank < old.rank) {
                old.rank = rank;
                old.weight = dict_.Weight(id);
                old.code = code;
            } else if (rank == old.rank && dict_.Weight(id) > old.weight) {
                old.weight = dict_.Weight(id);
                old.code = code;
            }
            return;
        }
        Candidate c;
        c.text = std::move(text);
        c.code = std::move(code);
        c.weight = dict_.Weight(id);
        c.rank = rank;
        auto l = learn_.find(c.text);
        c.learned = (l != learn_.end()) ? l->second : 0;
        if (!promo_.empty()) {
            auto p = promo_.find(PromoKey(buf_, c.text));
            if (p != promo_.end()) c.boost = p->second;
        }
        pos.emplace(c.text, tmp.size());
        tmp.push_back(std::move(c));
    };

    size_t prefixAdded = 0;
    for (uint32_t id : hits) {
        if (mode != Mode::Pinyin) {
            if (dict_.CodeView(id) != buf_) {
                if (mixed && prefixAdded >= kWubiPrefixCap) continue;
                ++prefixAdded;
            } else {
                ++wubiExact_;  // 四码精确命中的条数（唯一上屏只认它）
            }
            addHit(id, kRankWubiExact, kRankWubiPrefix, false);
        } else {
            addHit(id, kRankPyExact, kRankPyPrefix, true);
        }
    }
    if (mode != Mode::Pinyin) wubiCount_ = tmp.size();  // 纯拼音时没有五笔候选

    // 五笔拼音混输：五笔候选之后接拼音候选。会打五笔的照旧打五笔，
    // 忘了码直接拼也能出字；两者是同一份词库，重复的字会自动取靠前的名次。
    if (mode == Mode::Mixed && tmp.size() < 100) {
        std::vector<uint32_t> py;
        dict_.SearchPinyin(buf_, py, 60);
        pinyinHits_ = py.size();
        // 这串字母后面还能不能接着打拼音？判断依据：有没有哪条拼音候选的
        // 拼音比当前输入更长、且以当前输入开头（如 shiy → shi'yan 的“实验”）。
        // 有就说明用户可能还想继续拼，四码唯一上屏得让路。
        for (uint32_t id : py) {
            std::string_view pv = dict_.PyView(id);
            std::string code;
            code.reserve(pv.size());
            for (char ch : pv)
                if (ch != '\'') code.push_back(ch);
            if (code.size() > buf_.size() && code.compare(0, buf_.size(), buf_) == 0) {
                pinyinExtendable_ = true;
                break;
            }
        }
        for (uint32_t id : py) {
            addHit(id, kRankPyExact, kRankPyPrefix, true);
            if (tmp.size() >= 100) break;
        }
    }

    const bool freq = opt_.freqAdjust != 0;
    std::stable_sort(tmp.begin(), tmp.end(), [freq](const Candidate& a, const Candidate& b) {
        // 手动调频优先级最高：用户按 Ctrl+数字 指定过的候选一定排在本编码最前
        if (a.boost != b.boost) return a.boost > b.boost;
        if (a.rank != b.rank) return a.rank < b.rank;
        if (freq && a.learned != b.learned) return a.learned > b.learned;
        return a.weight > b.weight;
    });
    if (tmp.size() > 100) tmp.resize(100);
    cands_.swap(tmp);
}

void Engine::Learn(const std::wstring& text) {
    if (text.empty()) return;
    uint32_t& c = learn_[text];
    if (c < 100000) ++c;
    userDirty_ = true;
    if (learn_.size() > 8000) SaveUser();
}

void Engine::SaveUser() {
    if (!userDirty_) return;
    std::wstring path = dataDir_ + L"\\..\\user.dat";
    // 统一写到 %APPDATA%\QiuYuWubi\user.dat
    path = UserDir() + L"\\user.dat";
    std::string out;
    out.reserve(learn_.size() * 12);
    for (auto& kv : learn_) {
        out += WToUtf8(kv.first);
        out += '\t';
        out += std::to_string(kv.second);
        out += '\n';
    }
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h && h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        ::WriteFile(h, out.data(), (DWORD)out.size(), &written, nullptr);
        ::CloseHandle(h);
        userDirty_ = false;
    }
}

void Engine::LoadUser() {
    learn_.clear();
    std::wstring path = UserDir() + L"\\user.dat";
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!h || h == INVALID_HANDLE_VALUE) return;
    std::string data;
    char buf[8192];
    DWORD rd = 0;
    while (::ReadFile(h, buf, sizeof(buf), &rd, nullptr) && rd > 0) data.append(buf, rd);
    ::CloseHandle(h);
    size_t i = 0;
    while (i < data.size()) {
        size_t e = data.find('\n', i);
        if (e == std::string::npos) e = data.size();
        std::string line = data.substr(i, e - i);
        i = e + 1;
        size_t t = line.find('\t');
        if (t == std::string::npos) continue;
        std::wstring text = Utf8ToW(line.substr(0, t));
        uint32_t cnt = (uint32_t)atoi(line.c_str() + t + 1);
        if (!text.empty()) learn_[text] = cnt;
    }
    userDirty_ = false;
}

bool Engine::PromoteOnPage(size_t idxInPage) {
    size_t idx = page_ * (size_t)(opt_.pageSize > 0 ? opt_.pageSize : 5) + idxInPage;
    return PromoteGlobal(idx);
}

bool Engine::PromoteGlobal(size_t idx) {
    if (!opt_.manualFreq) return false;
    if (idx >= cands_.size()) return false;
    const std::wstring text = cands_[idx].text;
    if (text.empty()) return false;

    promo_[PromoKey(buf_, text)] = ++promoSeq_;

    // 记录过多时丢掉最早的四分之一，避免文件无限增长
    if (promo_.size() > 4000) {
        std::vector<uint32_t> seqs;
        seqs.reserve(promo_.size());
        for (auto& kv : promo_) seqs.push_back(kv.second);
        std::sort(seqs.begin(), seqs.end());
        uint32_t cut = seqs[seqs.size() / 4];
        for (auto it = promo_.begin(); it != promo_.end();) {
            if (it->second <= cut)
                it = promo_.erase(it);
            else
                ++it;
        }
    }

    // 立即重排：被调频的候选移到第一位，用户马上能看到效果
    Update();
    page_ = 0;
    SavePromote();
    return true;
}

void Engine::ClearPromotions(const std::string& code) {
    if (code.empty()) {
        promo_.clear();
    } else {
        std::wstring prefix = Utf8ToW(code);
        prefix.push_back(L'\x01');
        for (auto it = promo_.begin(); it != promo_.end();) {
            if (it->first.compare(0, prefix.size(), prefix) == 0)
                it = promo_.erase(it);
            else
                ++it;
        }
    }
    if (hasComp()) Update();
    SavePromote();
}

void Engine::SavePromote() {
    std::wstring path = UserDir() + L"\\promote.dat";
    std::string out;
    out.reserve(promo_.size() * 16);
    for (auto& kv : promo_) {
        // key = 编码 \x01 词条
        size_t sep = kv.first.find(L'\x01');
        if (sep == std::wstring::npos) continue;
        std::wstring code = kv.first.substr(0, sep);
        std::wstring text = kv.first.substr(sep + 1);
        if (text.empty()) continue;
        out += WToUtf8(code);
        out += '\t';
        out += WToUtf8(text);
        out += '\t';
        out += std::to_string(kv.second);
        out += '\n';
    }
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h && h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        ::WriteFile(h, out.data(), (DWORD)out.size(), &written, nullptr);
        ::CloseHandle(h);
        promoDirty_ = false;
    }
}

void Engine::LoadPromote() {
    promo_.clear();
    promoSeq_ = 0;
    std::wstring path = UserDir() + L"\\promote.dat";
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!h || h == INVALID_HANDLE_VALUE) return;
    std::string data;
    char buf[8192];
    DWORD rd = 0;
    while (::ReadFile(h, buf, sizeof(buf), &rd, nullptr) && rd > 0) data.append(buf, rd);
    ::CloseHandle(h);
    size_t i = 0;
    while (i < data.size()) {
        size_t e = data.find('\n', i);
        if (e == std::string::npos) e = data.size();
        std::string line = data.substr(i, e - i);
        i = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        size_t t2 = line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        std::string code = line.substr(0, t1);
        std::wstring text = Utf8ToW(line.substr(t1 + 1, t2 - t1 - 1));
        uint32_t seq = (uint32_t)strtoul(line.c_str() + t2 + 1, nullptr, 10);
        if (text.empty()) continue;
        promo_[PromoKey(code, text)] = seq;
        if (seq > promoSeq_) promoSeq_ = seq;
    }
    promoDirty_ = false;
}

std::wstring Engine::ToWidth(const std::wstring& s) const {    if (!opt_.fullWidth) return s;
    std::wstring r;
    r.reserve(s.size());
    for (wchar_t c : s) {
        if (c == L' ')
            r += L'\u3000';
        else if (c > 0x20 && c < 0x7F)
            r += (wchar_t)(c + 0xFEE0);
        else
            r += c;
    }
    return r;
}

std::wstring Engine::punct(wchar_t ch, bool shift) {
    // 半角状态且未启用中文标点时，若开了全角则输出全角符号
    if (!opt_.smartPunct) return ToWidth(std::wstring(1, ch));
    (void)shift;
    switch (ch) {
        case L',': return L"，";
        case L'.': return L"。";
        case L'?': return L"？";
        case L'!': return L"！";
        case L':': return L"：";
        case L';': return L"；";
        case L'\\': return L"、";
        case L'(': return L"（";
        case L')': return L"）";
        case L'[': return L"【";
        case L']': return L"】";
        case L'{': return L"『";
        case L'}': return L"』";
        case L'<': return L"《";
        case L'>': return L"》";
        case L'"': quoteOpen_ = !quoteOpen_; return quoteOpen_ ? L"“" : L"”";
        case L'\'': squoteOpen_ = !squoteOpen_; return squoteOpen_ ? L"‘" : L"’";
        case L'$': return L"￥";
        case L'@': return L"＠";
        case L'&': return L"＆";
        case L'*': return L"×";
        case L'/': return L"、";
        case L'-': return L"－";
        case L'_': return L"——";
        case L'+': return L"＋";
        case L'=': return L"＝";
        case L'~': return L"～";
        case L'`': return L"·";
        case L'^': return L"……";
        case L'%': return L"％";
        case L'#': return L"＃";
        default: return std::wstring(1, ch);
    }
}

}  // namespace qy
