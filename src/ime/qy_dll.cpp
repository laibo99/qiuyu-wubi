// 秋雨五笔输入法 - DLL 入口、类厂与系统注册
#define INITGUID
#include <initguid.h>
#include <msctf.h>
#include <olectl.h>
#include <new>
#include <string>

#include "qy_ime.h"

HINSTANCE g_hInst = nullptr;
static LONG g_cObj = 0;
static LONG g_cLock = 0;

namespace qy {
extern const wchar_t* kDisplayName = L"秋雨五笔输入法";
}

//------------------------------------------------------------------------------
class CClassFactory : public IClassFactory {
public:
    CClassFactory() : _cRef(1) { InterlockedIncrement(&g_cLock); }
    ~CClassFactory() { InterlockedDecrement(&g_cLock); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        *ppv = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
            *ppv = (IClassFactory*)this;
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&_cRef); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG c = InterlockedDecrement(&_cRef);
        if (c == 0) delete this;
        return c;
    }
    STDMETHODIMP CreateInstance(IUnknown* punkOuter, REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        *ppv = nullptr;
        if (punkOuter) return CLASS_E_NOAGGREGATION;
        qy::CTextService* p = new (std::nothrow) qy::CTextService();
        if (!p) return E_OUTOFMEMORY;
        InterlockedIncrement(&g_cObj);
        HRESULT hr = p->QueryInterface(riid, ppv);
        p->Release();
        if (FAILED(hr)) InterlockedDecrement(&g_cObj);
        return hr;
    }
    STDMETHODIMP LockServer(BOOL fLock) override {
        if (fLock)
            InterlockedIncrement(&g_cLock);
        else
            InterlockedDecrement(&g_cLock);
        return S_OK;
    }

private:
    LONG _cRef;
};

//------------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            g_hInst = hInst;
            ::DisableThreadLibraryCalls(hInst);
            break;
        case DLL_PROCESS_DETACH:
            break;
        default:
            break;
    }
    return TRUE;
}

extern "C" __declspec(dllexport) HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (!ppv) return E_INVALIDARG;
    *ppv = nullptr;
    if (!IsEqualCLSID(rclsid, qy::CLSID_QYTextService)) return CLASS_E_CLASSNOTAVAILABLE;
    CClassFactory* pFactory = new (std::nothrow) CClassFactory();
    if (!pFactory) return E_OUTOFMEMORY;
    HRESULT hr = pFactory->QueryInterface(riid, ppv);
    pFactory->Release();
    return hr;
}

extern "C" __declspec(dllexport) HRESULT STDAPICALLTYPE DllCanUnloadNow() {
    if (g_cObj == 0 && g_cLock == 0) return S_OK;
    return S_FALSE;
}

//------------------------------------------------------------------------------
// 注册辅助
namespace {

std::wstring ClsidString() {
    LPOLESTR s = nullptr;
    if (FAILED(::StringFromCLSID(qy::CLSID_QYTextService, &s)) || !s) return L"";
    std::wstring out(s);
    ::CoTaskMemFree(s);
    return out;
}

std::wstring GuidString(const GUID& g) {
    LPOLESTR s = nullptr;
    if (FAILED(::StringFromCLSID(g, &s)) || !s) return L"";
    std::wstring out(s);
    ::CoTaskMemFree(s);
    return out;
}

std::wstring ModulePath() {
    wchar_t buf[MAX_PATH * 2] = {0};
    ::GetModuleFileNameW(g_hInst, buf, MAX_PATH * 2);
    return std::wstring(buf);
}

bool SetKeyValue(HKEY root, const std::wstring& sub, const wchar_t* name, const std::wstring& val) {
    HKEY k = nullptr;
    DWORD disp = 0;
    if (::RegCreateKeyExW(root, sub.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, &disp) !=
        ERROR_SUCCESS)
        return false;
    LSTATUS st;
    if (name)
        st = ::RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)val.c_str(),
                              (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    else
        st = ::RegSetValueExW(k, nullptr, 0, REG_SZ, (const BYTE*)val.c_str(),
                              (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    ::RegCloseKey(k);
    return st == ERROR_SUCCESS;
}

bool SetKeyDword(HKEY root, const std::wstring& sub, const wchar_t* name, DWORD val) {
    HKEY k = nullptr;
    DWORD disp = 0;
    if (::RegCreateKeyExW(root, sub.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, &disp) !=
        ERROR_SUCCESS)
        return false;
    LSTATUS st = ::RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
    ::RegCloseKey(k);
    return st == ERROR_SUCCESS;
}

void DeleteKeyTree(HKEY root, const std::wstring& sub) {
    HKEY k = nullptr;
    if (::RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ | KEY_WRITE, &k) != ERROR_SUCCESS) return;
    // 递归删除子键
    wchar_t name[512];
    DWORD idx = 0;
    while (true) {
        DWORD len = 512;
        if (::RegEnumKeyExW(k, idx, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        ::RegCloseKey(k);
        DeleteKeyTree(root, sub + L"\\" + name);
        if (::RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ | KEY_WRITE, &k) != ERROR_SUCCESS)
            return;
    }
    ::RegCloseKey(k);
    ::RegDeleteKeyW(root, sub.c_str());
}

// 注册一个语言配置
bool RegisterProfile(HKEY root, const std::wstring& tipSub, const GUID& profile,
                     const std::wstring& desc, const std::wstring& icon) {
    std::wstring sub = tipSub + L"\\LanguageProfile\\0x00000804\\" + GuidString(profile);
    if (!SetKeyValue(root, sub, nullptr, desc)) return false;
    SetKeyValue(root, sub, L"Description", desc);
    SetKeyValue(root, sub, L"IconFile", icon);
    SetKeyDword(root, sub, L"IconIndex", 0);
    SetKeyDword(root, sub, L"Enable", 1);
    // 语言配置父键
    SetKeyDword(root, tipSub + L"\\LanguageProfile\\0x00000804", L"Enable", 1);
    return true;
}

// 把输入法加入"用户语言列表"，使其出现在语言栏与 Win+Space 切换列表
bool RegisterUserLanguageProfile(const GUID& profile, const std::wstring& clsidStr) {
    std::wstring valueName = L"0804:" + clsidStr + GuidString(profile);
    std::wstring sub = L"Control Panel\\International\\User Profile\\zh-Hans-CN";
    HKEY k = nullptr;
    DWORD disp = 0;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k,
                          &disp) != ERROR_SUCCESS)
        return false;
    DWORD one = 1;
    LSTATUS st = ::RegSetValueExW(k, valueName.c_str(), 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
    ::RegCloseKey(k);
    return st == ERROR_SUCCESS;
}

void UnregisterUserLanguageProfile(const GUID& profile, const std::wstring& clsidStr) {
    std::wstring sub = L"Control Panel\\International\\User Profile\\zh-Hans-CN";
    HKEY k = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        std::wstring v1 = L"0804:" + clsidStr + GuidString(profile);
        std::wstring v2 = L"0804" + clsidStr + GuidString(profile);
        ::RegDeleteValueW(k, v1.c_str());
        ::RegDeleteValueW(k, v2.c_str());
        ::RegCloseKey(k);
    }
}

void UnregisterProfile(HKEY root, const std::wstring& tipSub, const GUID& profile) {
    DeleteKeyTree(root, tipSub + L"\\LanguageProfile\\0x00000804\\" + GuidString(profile));
}

}  // namespace

//------------------------------------------------------------------------------
extern "C" __declspec(dllexport) HRESULT STDAPICALLTYPE DllRegisterServer() {
    HRESULT hrCo = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;

    std::wstring clsid = ClsidString();
    std::wstring dll = ModulePath();
    std::wstring dir = dll.substr(0, dll.find_last_of(L"\\/"));
    std::wstring icon = dir + L"\\qywb.ico";

    for (int pass = 0; pass < 2 && !ok; ++pass) {
        HKEY root = (pass == 0) ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
        std::wstring classes = (pass == 0) ? (L"SOFTWARE\\Classes\\CLSID\\" + clsid)
                                           : (L"Software\\Classes\\CLSID\\" + clsid);
        std::wstring tipSub = (pass == 0) ? (L"SOFTWARE\\Microsoft\\CTF\\TIP\\" + clsid)
                                          : (L"Software\\Microsoft\\CTF\\TIP\\" + clsid);

        SetKeyValue(root, tipSub, nullptr, qy::kDisplayName);
        SetKeyDword(root, tipSub, L"Enable", 1);  // TSF 要求 TIP 主键显式启用
        bool r1 = SetKeyValue(root, classes, nullptr, qy::kDisplayName);
        bool r2 = SetKeyValue(root, classes + L"\\InprocServer32", nullptr, dll);
        bool r3 = SetKeyValue(root, classes + L"\\InprocServer32", L"ThreadingModel", L"Apartment");
        // 只注册一个语言配置：五笔与拼音在同一输入法内切换
        bool r4 = RegisterProfile(root, tipSub, qy::GUID_QYProfileWubi, L"秋雨五笔", icon);
        // 清理旧版遗留的“秋雨拼音”独立方案，避免语言列表出现两项
        UnregisterProfile(root, tipSub, qy::GUID_QYProfilePinyin);
        ok = r1 && r2 && r3 && r4;
    }

    // 旧版把拼音注册成独立方案，这里彻底移除其用户语言列表项
    UnregisterUserLanguageProfile(qy::GUID_QYProfilePinyin, clsid);
    RegisterUserLanguageProfile(qy::GUID_QYProfileWubi, clsid);

    // 分类注册（语言栏分类），失败不影响基本可用
    ITfCategoryMgr* pCat = nullptr;
    if (SUCCEEDED(::CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                     IID_ITfCategoryMgr, (void**)&pCat)) &&
        pCat) {
        pCat->RegisterCategory(qy::CLSID_QYTextService, GUID_TFCAT_TIP_KEYBOARD,
                               qy::CLSID_QYTextService);
        pCat->Release();
    }

    // 通过 API 启用语言配置（只保留五笔一项）
    ITfInputProcessorProfiles* pProfiles = nullptr;
    if (SUCCEEDED(::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                     IID_ITfInputProcessorProfiles, (void**)&pProfiles)) &&
        pProfiles) {
        // 先移除旧版的拼音方案（存在时返回 S_OK，不存在返回错误，忽略即可）
        pProfiles->RemoveLanguageProfile(qy::CLSID_QYTextService, 0x0804,
                                         qy::GUID_QYProfilePinyin);
        pProfiles->AddLanguageProfile(qy::CLSID_QYTextService, 0x0804, qy::GUID_QYProfileWubi,
                                      L"秋雨五笔", 4, icon.c_str(), (ULONG)icon.size(), 0);
        pProfiles->EnableLanguageProfile(qy::CLSID_QYTextService, 0x0804, qy::GUID_QYProfileWubi,
                                         TRUE);
        pProfiles->Release();
    }

    if (hrCo == S_OK || hrCo == S_FALSE) ::CoUninitialize();
    return ok ? S_OK : SELFREG_E_CLASS;
}

extern "C" __declspec(dllexport) HRESULT STDAPICALLTYPE DllUnregisterServer() {
    HRESULT hrCo = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::wstring clsid = ClsidString();
    UnregisterUserLanguageProfile(qy::GUID_QYProfileWubi, clsid);
    UnregisterUserLanguageProfile(qy::GUID_QYProfilePinyin, clsid);
    for (int pass = 0; pass < 2; ++pass) {
        HKEY root = (pass == 0) ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
        std::wstring classes = (pass == 0) ? (L"SOFTWARE\\Classes\\CLSID\\" + clsid)
                                           : (L"Software\\Classes\\CLSID\\" + clsid);
        std::wstring tipSub = (pass == 0) ? (L"SOFTWARE\\Microsoft\\CTF\\TIP\\" + clsid)
                                          : (L"Software\\Microsoft\\CTF\\TIP\\" + clsid);
        UnregisterProfile(root, tipSub, qy::GUID_QYProfileWubi);
        UnregisterProfile(root, tipSub, qy::GUID_QYProfilePinyin);
        // 连同 TIP 主键（CLSID / Enable / 0x00000804 容器）一并清除，避免残留
        DeleteKeyTree(root, tipSub);
        DeleteKeyTree(root, classes);
    }
    if (hrCo == S_OK || hrCo == S_FALSE) ::CoUninitialize();
    return S_OK;
}
