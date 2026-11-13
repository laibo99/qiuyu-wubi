// 秋雨五笔输入法 - 码表加载与检索实现
#include "qy_dict.h"
#include <algorithm>
#include <utility>

namespace qy {

namespace {
struct Header {
    char magic[8];
    uint32_t version;
    uint32_t count;
    uint32_t wcharOff, wcharLen;
    uint32_t charOff, charLen;
    uint32_t codeOff, codeLen;
    uint32_t codeIdxOff;
    uint32_t pyIdxOff, pyIdxCount;
    uint32_t abbrIdxOff, abbrIdxCount;
    uint32_t textIdxOff, textIdxCount;
    uint32_t reserved;
};
}  // namespace

Dict::~Dict() {
    if (base_) ::UnmapViewOfFile(base_);
    if (hMap_) ::CloseHandle(hMap_);
    if (hFile_ != INVALID_HANDLE_VALUE && hFile_) ::CloseHandle(hFile_);
}

bool Dict::Load(const std::wstring& datPath) {
    hFile_ = ::CreateFileW(datPath.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile_ == INVALID_HANDLE_VALUE || !hFile_) return false;
    LARGE_INTEGER li{};
    if (!::GetFileSizeEx(hFile_, &li)) return false;
    size_ = (size_t)li.QuadPart;
    if (size_ < sizeof(Header)) return false;
    hMap_ = ::CreateFileMappingW(hFile_, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!hMap_) return false;
    base_ = (const uint8_t*)::MapViewOfFile(hMap_, FILE_MAP_READ, 0, 0, 0);
    if (!base_) return false;

    const Header* h = (const Header*)base_;
    if (memcmp(h->magic, "QYWBDIC1", 8) != 0 || h->version != 1) return false;
    count_ = h->count;
    const uint8_t* p = base_;
    recs_ = (const DictRec*)(p + sizeof(Header));
    wtext_ = (const wchar_t*)(p + h->wcharOff);
    chars_ = (const char*)(p + h->charOff);
    codes_ = (const char*)(p + h->codeOff);
    codeIdx_ = (const uint32_t*)(p + h->codeIdxOff);
    pyIdx_ = (const uint32_t*)(p + h->pyIdxOff);
    pyCount_ = h->pyIdxCount;
    abbrIdx_ = (const uint32_t*)(p + h->abbrIdxOff);
    abbrCount_ = h->abbrIdxCount;
    textIdx_ = (const uint32_t*)(p + h->textIdxOff);
    textCount_ = h->textIdxCount;
    // 完整性检查
    if (h->wcharOff + h->wcharLen * 2 > size_) return false;
    if (h->charOff + h->charLen > size_) return false;
    if (h->codeOff + h->codeLen > size_) return false;
    if (h->pyIdxOff + pyCount_ * 4 > size_) return false;
    if (h->abbrIdxOff + abbrCount_ * 4 > size_) return false;
    if (h->textIdxOff + textCount_ * 4 > size_) return false;
    return true;
}

bool Dict::WildMatch(std::string_view code, std::string_view pat) {
    if (code.size() < pat.size()) return false;
    for (size_t i = 0; i < pat.size(); ++i) {
        char pc = pat[i];
        if (pc == 'z') continue;
        if (code[i] != pc) return false;
    }
    return true;
}

// 音节匹配：input 需被音节序列完整消费（每个音节可全拼、首字母或末尾部分拼写）
bool Dict::SylMatch(const char* syl, int sylCount, const char* in, int inLen) {
    int ii = 0;
    for (int si = 0; si < sylCount; ++si) {
        const char* s = syl;
        while (*s && *s != '\'') ++s;
        int slen = (int)(s - syl);
        ++s;  // 跳过分隔符
        if (ii >= inLen) return false;
        int rest = inLen - ii;
        // 全拼 / 末尾部分拼写
        int cmp = rest < slen ? rest : slen;
        bool prefixOk = true;
        for (int k = 0; k < cmp; ++k) {
            char a = syl[k], b = in[ii + k];
            if (a == 'v' && b == 'u') continue;  // ü 兼容 u
            if (a != b) { prefixOk = false; break; }
        }
        if (prefixOk) {
            if (cmp == slen) {
                // 完整消费该音节
                if (si + 1 == sylCount) {
                    if (rest == slen) return true;  // 输入用尽且音节用尽
                } else {
                    if (SylMatch(s, sylCount - si - 1, in + ii + slen, rest - slen)) return true;
                }
            } else if (cmp == rest && si + 1 == sylCount) {
                return true;  // 末音节部分拼写
            }
        }
        // 首字母缩写
        if (rest >= 1) {
            char b = in[ii];
            bool ok = (syl[0] == b) || (syl[0] == 'v' && b == 'u');
            if (ok) {
                if (si + 1 == sylCount) {
                    if (rest == 1) return true;
                } else {
                    if (SylMatch(s, sylCount - si - 1, in + ii + 1, rest - 1)) return true;
                }
            }
        }
    }
    return false;
}

void Dict::SearchWubi(const std::string& input, std::vector<uint32_t>& out, size_t limit) const {
    out.clear();
    if (!loaded() || input.empty()) return;
    const size_t maxOut = limit ? limit * 4 + 16 : 400;

    if (input.find('z') != std::string::npos) {
        // 通配分支（码里有 z，z 是五笔万能键）。
        // 注意两点：
        //   1) 精确码要优先 —— 自定义词条（如 wxzf、jjz）与普通词条可能同码，
        //      精确的排前面；
        //   2) 不能全表扫描后排序 —— 含 z 的输入（z、zhong…）会明显变慢，
        //      实测每串超过 10ms（阈值 8ms）。所以先用二分把精确码捞出来，
        //      通配部分保持“扫到够数就停”。
        std::string_view pat(input);
        // 精确码：码索引按 (码, 权重降序) 排序，同码的记录是连续的
        size_t lo = 0, hi = count_;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            std::string_view key = CodeView(codeIdx_[mid]);
            if (key < pat)
                lo = mid + 1;
            else
                hi = mid;
        }
        for (size_t i = lo; i < count_ && out.size() < maxOut; ++i) {
            uint32_t idx = codeIdx_[i];
            if (CodeView(idx) != pat) break;
            out.push_back(idx);
        }
        // 通配匹配：命中数够了就停（保持与旧实现一致的速度）
        size_t exactCount = out.size();
        for (size_t i = 0; i < count_ && out.size() < maxOut; ++i) {
            uint32_t idx = codeIdx_[i];
            std::string_view cv = CodeView(idx);
            if (cv == pat) continue;  // 精确的已经收好了
            if (WildMatch(cv, pat)) out.push_back(idx);
        }
        (void)exactCount;
        return;
    }

    // 二分查找前缀起点
    size_t lo = 0, hi = count_;
    std::string_view key(input);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        std::string_view c = CodeView(codeIdx_[mid]);
        int cmp = c.compare(0, c.size() < key.size() ? c.size() : key.size(), key);
        if (cmp < 0 || (cmp == 0 && c.size() < key.size()))
            lo = mid + 1;
        else
            hi = mid;
    }
    for (size_t i = lo; i < count_ && out.size() < maxOut; ++i) {
        uint32_t idx = codeIdx_[i];
        std::string_view c = CodeView(idx);
        if (c.size() < key.size()) continue;
        if (c.compare(0, key.size(), key) != 0) break;
        out.push_back(idx);
    }
}

void Dict::SearchPinyin(const std::string& input, std::vector<uint32_t>& out, size_t limit) const {
    out.clear();
    if (!loaded() || input.empty()) return;
    const size_t maxOut = limit ? limit * 6 + 24 : 400;
    char first = input[0];

    // 拼音索引按紧凑拼音排序，取首字母相同的连续区间
    size_t lo = 0, hi = pyCount_;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        std::wstring_view tv = TextView(pyIdx_[mid]);
        (void)tv;
        std::string_view py = PyView(pyIdx_[mid]);
        if (py.empty() || py[0] < first)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (size_t i = lo; i < pyCount_ && out.size() < maxOut; ++i) {
        uint32_t idx = pyIdx_[i];
        std::string_view py = PyView(idx);
        if (py.empty() || py[0] != first) break;
        if (SylMatch(py.data(), (int)recs_[idx].sylCount, input.c_str(), (int)input.size()))
            out.push_back(idx);
    }
}

}  // namespace qy
