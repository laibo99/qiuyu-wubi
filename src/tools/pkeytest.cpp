// 自检：验证 TSF 的「键盘开/关隔间」与「保留键」两条通道可用
// 1) GUID_COMPARTMENT_KEYBOARD_OPENCLOSE 能读、能写、能收到变更通知
// 2) ITfKeystrokeMgr::PreserveKey(Ctrl+Space) 能被系统接受（IsPreservedKey 为真）
// 结果写入 build/pkey_out.txt
#define INITGUID
#include <initguid.h>
#include <windows.h>
#include <msctf.h>
#include <stdio.h>

static const GUID kTestKey = {0x7e2f1c4a, 0x9b3d, 0x4a6e,
                              {0x8f, 0x52, 0x1c, 0x3d, 0x5a, 0x7b, 0x9e, 0x05}};

static FILE* g_out = nullptr;
static int g_changes = 0;

class Sink : public ITfCompartmentEventSink {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        *ppv = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfCompartmentEventSink)) {
            *ppv = (ITfCompartmentEventSink*)this;
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&_c); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG c = InterlockedDecrement(&_c);
        if (c == 0) delete this;
        return c;
    }
    STDMETHODIMP OnChange(REFGUID rguid) override {
        if (IsEqualGUID(rguid, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE)) {
            ++g_changes;
            fprintf(g_out, "  -> OnChange OPENCLOSE fired\n");
        }
        return S_OK;
    }

private:
    LONG _c = 1;
};

static void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_out, fmt, ap);
    va_end(ap);
    fputc('\n', g_out);
    fflush(g_out);
}

int main() {
    g_out = fopen("build/pkey_out.txt", "wb");
    if (!g_out) return 1;

    HRESULT hrCo = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Log("CoInitialize hr=0x%08lx", (unsigned long)hrCo);

    ITfThreadMgr* tim = nullptr;
    HRESULT hr = ::CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_ITfThreadMgr, (void**)&tim);
    if (FAILED(hr) || !tim) {
        Log("FAIL CoCreateThreadMgr hr=0x%08lx", (unsigned long)hr);
        fclose(g_out);
        return 1;
    }
    TfClientId tid = 0;
    hr = tim->Activate(&tid);
    Log("Activate hr=0x%08lx tid=%lu", (unsigned long)hr, (unsigned long)tid);

    // --- 1. 隔间 ---
    ITfCompartmentMgr* cm = nullptr;
    hr = tim->QueryInterface(IID_ITfCompartmentMgr, (void**)&cm);
    ITfCompartment* comp = nullptr;
    if (SUCCEEDED(hr) && cm) {
        hr = cm->GetCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, &comp);
        Log("GetCompartment(OPENCLOSE) hr=0x%08lx comp=%p", (unsigned long)hr, (void*)comp);
    } else {
        Log("FAIL QueryInterface(ITfCompartmentMgr) hr=0x%08lx", (unsigned long)hr);
    }

    if (comp) {
        VARIANT v;
        ::VariantInit(&v);
        hr = comp->GetValue(&v);
        Log("  GetValue hr=0x%08lx vt=%d value=%ld", (unsigned long)hr, v.vt,
            v.vt == VT_I4 ? v.lVal : -999);
        int before = (v.vt == VT_I4) ? v.lVal : -1;
        ::VariantClear(&v);

        // 变更通知
        Sink* sink = new Sink();
        ITfSource* src = nullptr;
        DWORD cookie = TF_INVALID_COOKIE;
        hr = comp->QueryInterface(IID_ITfSource, (void**)&src);
        if (SUCCEEDED(hr) && src) {
            hr = src->AdviseSink(IID_ITfCompartmentEventSink, (ITfCompartmentEventSink*)sink,
                                 &cookie);
            Log("  AdviseSink hr=0x%08lx cookie=%lu", (unsigned long)hr,
                (unsigned long)cookie);
        }

        // 写入 + 回读
        VARIANT w;
        ::VariantInit(&w);
        w.vt = VT_I4;
        w.lVal = 0;
        hr = comp->SetValue(tid, &w);
        Log("  SetValue(0) hr=0x%08lx", (unsigned long)hr);
        ::VariantClear(&w);

        MSG msg;
        DWORD end = ::GetTickCount() + 300;
        while (::GetTickCount() < end) {
            while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                ::TranslateMessage(&msg);
                ::DispatchMessageW(&msg);
            }
            ::Sleep(10);
        }
        ::VariantInit(&v);
        if (SUCCEEDED(comp->GetValue(&v)) && v.vt == VT_I4)
            Log("  after SetValue(0) -> %ld", v.lVal);
        ::VariantClear(&v);
        Log("  OnChange count=%d", g_changes);

        // 还原
        ::VariantInit(&w);
        w.vt = VT_I4;
        w.lVal = before >= 0 ? before : 1;
        comp->SetValue(tid, &w);
        ::VariantClear(&w);

        if (src && cookie != TF_INVALID_COOKIE) src->UnadviseSink(cookie);
        if (src) src->Release();
        sink->Release();
    }

    // --- 2. 保留键 ---
    ITfKeystrokeMgr* km = nullptr;
    hr = tim->QueryInterface(IID_ITfKeystrokeMgr, (void**)&km);
    if (SUCCEEDED(hr) && km) {
        TF_PRESERVEDKEY pk{};
        pk.uVKey = VK_SPACE;
        pk.uModifiers = TF_MOD_CONTROL;
        const wchar_t* desc = L"秋雨五笔 中英文切换";
        Log("PreserveKey(Ctrl+Space) hr=0x%08lx", (unsigned long)hr);
        BOOL have = FALSE;
        hr = km->IsPreservedKey(kTestKey, &pk, &have);
        Log("IsPreservedKey hr=0x%08lx registered=%d", (unsigned long)hr, (int)have);
        km->UnpreserveKey(kTestKey, &pk);

        // 对照：换一个没有被系统占用的组合，确认 PreserveKey 本身可用
        TF_PRESERVEDKEY pk2{};
        pk2.uVKey = VK_F12;
        pk2.uModifiers = TF_MOD_CONTROL;
        hr = km->PreserveKey(tid, kTestKey, &pk2, desc, (ULONG)wcslen(desc));
        Log("PreserveKey(Ctrl+F12) hr=0x%08lx", (unsigned long)hr);
        BOOL have2 = FALSE;
        km->IsPreservedKey(kTestKey, &pk2, &have2);
        Log("IsPreservedKey(Ctrl+F12) registered=%d", (int)have2);
        km->UnpreserveKey(kTestKey, &pk2);
        km->Release();
    } else {
        Log("FAIL QueryInterface(ITfKeystrokeMgr) hr=0x%08lx", (unsigned long)hr);
    }

    if (comp) comp->Release();
    if (cm) cm->Release();
    tim->Deactivate();
    tim->Release();
    ::CoUninitialize();

    int fail = 0;
    fclose(g_out);
    return fail;
}
