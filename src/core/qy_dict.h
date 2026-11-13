// 秋雨五笔输入法 - 码表加载与检索
#pragma once
#include "qy_common.h"
#include <string_view>

namespace qy {

struct DictRec {  // 20 字节，与 build_dict.py 保持一致
    uint32_t textOff;
    uint32_t pyOff;
    uint32_t codeOff;
    uint16_t textLen;
    uint8_t codeLen;
    uint8_t sylCount;
    uint32_t weight;
};

class Dict {
public:
    Dict() = default;
    ~Dict();

    bool Load(const std::wstring& datPath);
    bool loaded() const { return base_ != nullptr; }
    size_t Count() const { return count_; }

    std::wstring Text(uint32_t i) const {
        const DictRec& r = rec(i);
        return std::wstring(wtext_ + r.textOff, r.textLen);
    }
    std::wstring_view TextView(uint32_t i) const {
        const DictRec& r = rec(i);
        return std::wstring_view(wtext_ + r.textOff, r.textLen);
    }
    std::string_view CodeView(uint32_t i) const {
        const DictRec& r = rec(i);
        return std::string_view(codes_ + r.codeOff, r.codeLen);
    }
    std::string_view PyView(uint32_t i) const {  // 音节以 ' 分隔，末尾带分隔符
        const DictRec& r = rec(i);
        if (r.sylCount == 0) return std::string_view();
        const char* p = chars_ + r.pyOff;
        const char* q = p;
        for (int syl = 0; syl < r.sylCount; ++syl) {
            while (*q && *q != '\'') ++q;
            ++q;  // 跳过分隔符
        }
        return std::string_view(p, (size_t)(q - p - 1));
    }
    uint32_t Weight(uint32_t i) const { return rec(i).weight; }

    // 五笔检索：返回按"编码精确优先、权重降序"的候选记录下标
    void SearchWubi(const std::string& input, std::vector<uint32_t>& out, size_t limit) const;
    // 拼音检索：支持全拼、简拼、混合
    void SearchPinyin(const std::string& input, std::vector<uint32_t>& out, size_t limit) const;

    const wchar_t* TextPtr(uint32_t i) const { return wtext_ + rec(i).textOff; }
    uint32_t TextLen(uint32_t i) const { return rec(i).textLen; }

private:
    const DictRec& rec(uint32_t i) const { return recs_[i]; }
    static bool WildMatch(std::string_view code, std::string_view pat);
    static bool SylMatch(const char* syl, int sylCount, const char* in, int inLen);

    HANDLE hFile_ = nullptr;
    HANDLE hMap_ = nullptr;
    const uint8_t* base_ = nullptr;
    size_t size_ = 0;
    const DictRec* recs_ = nullptr;
    const wchar_t* wtext_ = nullptr;
    const char* chars_ = nullptr;
    const char* codes_ = nullptr;
    const uint32_t* codeIdx_ = nullptr;
    const uint32_t* pyIdx_ = nullptr;
    const uint32_t* abbrIdx_ = nullptr;
    const uint32_t* textIdx_ = nullptr;
    size_t count_ = 0, pyCount_ = 0, abbrCount_ = 0, textCount_ = 0;
};

}  // namespace qy
