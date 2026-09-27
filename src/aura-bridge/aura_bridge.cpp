#include "aura_bridge.h"

#include <windows.h>
#include <oleauto.h>

#include <cstdio>
#include <initializer_list>
#include <utility>

#include "log.h"

namespace luma {
namespace {

// Minimal IDispatch smart pointer (avoids a dependency on WRL / comdef).
class Disp {
public:
    Disp() = default;
    explicit Disp(IDispatch* p) : p_(p) {}  // takes ownership of one reference
    Disp(const Disp& o) : p_(o.p_) {
        if (p_) p_->AddRef();
    }
    Disp(Disp&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    Disp& operator=(Disp o) {
        std::swap(p_, o.p_);
        return *this;
    }
    ~Disp() {
        if (p_) p_->Release();
    }
    IDispatch* get() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    IDispatch** out() {
        *this = Disp();
        return &p_;
    }

private:
    IDispatch* p_ = nullptr;
};

class Variant {
public:
    Variant() { VariantInit(&v); }
    ~Variant() { VariantClear(&v); }
    Variant(const Variant&) = delete;
    Variant& operator=(const Variant&) = delete;
    VARIANT v;
};

// Invokes a member by name. `args` are given in natural (left-to-right) order.
HRESULT Invoke(IDispatch* obj, const wchar_t* name, WORD flags, std::initializer_list<VARIANT> args,
               VARIANT* result) {
    if (!obj) return E_POINTER;
    DISPID id;
    LPOLESTR names[] = {const_cast<LPOLESTR>(name)};
    HRESULT hr = obj->GetIDsOfNames(IID_NULL, names, 1, LOCALE_USER_DEFAULT, &id);
    if (FAILED(hr)) return hr;

    // DISPPARAMS wants arguments in reverse order.
    VARIANT rev[4];
    UINT n = 0;
    for (auto it = args.end(); it != args.begin() && n < 4;) rev[n++] = *--it;

    DISPID putId = DISPID_PROPERTYPUT;
    DISPPARAMS dp{};
    dp.rgvarg = n ? rev : nullptr;
    dp.cArgs = n;
    if (flags & DISPATCH_PROPERTYPUT) {
        dp.rgdispidNamedArgs = &putId;
        dp.cNamedArgs = 1;
    }
    EXCEPINFO ei{};
    UINT argErr = 0;
    hr = obj->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, flags, &dp, result, &ei, &argErr);
    if (ei.bstrSource) SysFreeString(ei.bstrSource);
    if (ei.bstrDescription) SysFreeString(ei.bstrDescription);
    if (ei.bstrHelpFile) SysFreeString(ei.bstrHelpFile);
    return hr;
}

constexpr WORD kGet = DISPATCH_METHOD | DISPATCH_PROPERTYGET;

VARIANT MakeI4(LONG v) {
    VARIANT x;
    VariantInit(&x);
    x.vt = VT_I4;
    x.lVal = v;
    return x;
}

VARIANT MakeUI4(ULONG v) {
    VARIANT x;
    VariantInit(&x);
    x.vt = VT_UI4;
    x.ulVal = v;
    return x;
}

HRESULT GetDispObject(IDispatch* obj, const wchar_t* name, std::initializer_list<VARIANT> args,
                  Disp* out) {
    Variant r;
    HRESULT hr = Invoke(obj, name, kGet, args, &r.v);
    if (FAILED(hr)) return hr;
    if (r.v.vt != VT_DISPATCH || !r.v.pdispVal) return E_NOINTERFACE;
    r.v.pdispVal->AddRef();
    *out = Disp(r.v.pdispVal);
    return S_OK;
}

HRESULT GetLong(IDispatch* obj, const wchar_t* name, long* out) {
    Variant r;
    HRESULT hr = Invoke(obj, name, kGet, {}, &r.v);
    if (FAILED(hr)) return hr;
    hr = VariantChangeType(&r.v, &r.v, 0, VT_I4);
    if (FAILED(hr)) return hr;
    *out = r.v.lVal;
    return S_OK;
}

HRESULT GetString(IDispatch* obj, const wchar_t* name, std::wstring* out) {
    Variant r;
    HRESULT hr = Invoke(obj, name, kGet, {}, &r.v);
    if (FAILED(hr)) return hr;
    hr = VariantChangeType(&r.v, &r.v, 0, VT_BSTR);
    if (FAILED(hr)) return hr;
    *out = r.v.bstrVal ? std::wstring(r.v.bstrVal, SysStringLen(r.v.bstrVal)) : L"";
    return S_OK;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0,
                                nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr,
                        nullptr);
    return s;
}

struct Device {
    AuraDeviceInfo info;
    Disp device;
    std::vector<Disp> lights;
    bool selected = false;
};

}  // namespace

const wchar_t* AuraDeviceTypeName(uint32_t type) {
    switch (type) {
    case 0x00010000: return L"Motherboard";
    case 0x00011000: return L"Motherboard LED strip";
    case 0x00020000: return L"All-in-one PC";
    case 0x00030000: return L"VGA";
    case 0x00040000: return L"Display";
    case 0x00050000: return L"Headset";
    case 0x00060000: return L"Microphone";
    case 0x00070000: return L"External HDD";
    case 0x00080000: return L"External BD drive";
    case 0x00090000: return L"DRAM";
    case 0x000A0000: return L"Keyboard";
    case 0x000A0001: return L"Notebook keyboard";
    case 0x000A0011: return L"Notebook 4-zone keyboard";
    case 0x000B0000: return L"Mouse";
    case 0x000C0000: return L"Chassis";
    case 0x000D0000: return L"Projector";
    default: return L"Unknown";
    }
}

struct AuraBridge::Impl {
    Disp sdk;
    std::vector<Device> devices;
    std::vector<AuraDeviceInfo> infos;
    bool connected = false;
};

AuraBridge::AuraBridge() : impl_(new Impl) {}

AuraBridge::~AuraBridge() { Disconnect(false); }

bool AuraBridge::IsConnected() const { return impl_->connected; }

const std::vector<AuraDeviceInfo>& AuraBridge::Devices() const { return impl_->infos; }

bool AuraBridge::Connect(const DeviceFilter& filter) {
    Disconnect(false);

    CLSID clsid;
    HRESULT hr = CLSIDFromProgID(L"aura.sdk.1", &clsid);
    if (FAILED(hr)) {
        LUMA_WARN("Aura: ProgID aura.sdk.1 not registered (hr=0x%08lX) - is Armoury Crate / "
                  "Aura LightingService installed?",
                  static_cast<unsigned long>(hr));
        return false;
    }
    hr = CoCreateInstance(clsid, nullptr, CLSCTX_ALL, IID_IDispatch,
                          reinterpret_cast<void**>(impl_->sdk.out()));
    if (FAILED(hr)) {
        LUMA_WARN("Aura: CoCreateInstance failed (hr=0x%08lX) - is LightingService running?",
                  static_cast<unsigned long>(hr));
        return false;
    }

    hr = Invoke(impl_->sdk.get(), L"SwitchMode", DISPATCH_METHOD, {}, nullptr);
    if (FAILED(hr)) {
        LUMA_WARN("Aura: SwitchMode failed (hr=0x%08lX)", static_cast<unsigned long>(hr));
        impl_->sdk = Disp();
        return false;
    }

    Disp collection;
    hr = GetDispObject(impl_->sdk.get(), L"Enumerate", {MakeUI4(0)}, &collection);  // 0 = all types
    if (FAILED(hr)) {
        LUMA_WARN("Aura: Enumerate failed (hr=0x%08lX)", static_cast<unsigned long>(hr));
        impl_->sdk = Disp();
        return false;
    }

    long count = 0;
    GetLong(collection.get(), L"Count", &count);
    LUMA_INFO("Aura: %ld device(s) enumerated", count);

    for (long i = 0; i < count; ++i) {
        Device d;
        if (FAILED(GetDispObject(collection.get(), L"Item", {MakeI4(i)}, &d.device))) {
            LUMA_WARN("Aura: could not get device %ld", i);
            continue;
        }
        long type = 0, w = 0, h = 0;
        GetString(d.device.get(), L"Name", &d.info.name);
        GetLong(d.device.get(), L"Type", &type);
        GetLong(d.device.get(), L"Width", &w);   // optional; ignore failure
        GetLong(d.device.get(), L"Height", &h);
        d.info.type = static_cast<uint32_t>(type);
        d.info.width = static_cast<int>(w);
        d.info.height = static_cast<int>(h);

        Disp lights;
        long lightCount = 0;
        if (SUCCEEDED(GetDispObject(d.device.get(), L"Lights", {}, &lights)))
            GetLong(lights.get(), L"Count", &lightCount);
        for (long j = 0; j < lightCount; ++j) {
            Disp light;
            if (SUCCEEDED(GetDispObject(lights.get(), L"Item", {MakeI4(j)}, &light)))
                d.lights.push_back(light);
        }
        d.info.lightCount = static_cast<int>(d.lights.size());
        d.selected = !filter || filter(d.info);

        LUMA_INFO("Aura:   [%ld] \"%s\" type=0x%08lX (%s) lights=%d %dx%d%s", i,
                  Narrow(d.info.name).c_str(), static_cast<unsigned long>(d.info.type),
                  Narrow(AuraDeviceTypeName(d.info.type)).c_str(), d.info.lightCount,
                  d.info.width, d.info.height, d.selected ? "" : " (excluded)");

        impl_->infos.push_back(d.info);
        impl_->devices.push_back(std::move(d));
    }

    impl_->connected = true;
    return true;
}

void AuraBridge::Disconnect(bool releaseControl) {
    if (!impl_) return;
    impl_->devices.clear();
    impl_->infos.clear();
    if (impl_->sdk && releaseControl) {
        HRESULT hr =
            Invoke(impl_->sdk.get(), L"ReleaseControl", DISPATCH_METHOD, {MakeUI4(0)}, nullptr);
        if (FAILED(hr))
            LUMA_WARN("Aura: ReleaseControl failed (hr=0x%08lX)", static_cast<unsigned long>(hr));
        else
            LUMA_INFO("Aura: control released back to Armoury Crate");
    }
    impl_->sdk = Disp();
    impl_->connected = false;
}

void AuraBridge::SetSelected(size_t index, bool selected) {
    if (index >= impl_->devices.size()) return;
    Device& d = impl_->devices[index];
    if (d.selected != selected)
        LUMA_INFO("Aura: \"%s\" %s", Narrow(d.info.name).c_str(), selected ? "enabled" : "disabled");
    d.selected = selected;
}

bool AuraBridge::IsSelected(size_t index) const {
    return index < impl_->devices.size() && impl_->devices[index].selected;
}

bool AuraBridge::SetAll(uint32_t auraColor) {
    if (!impl_->connected) return false;
    for (auto& d : impl_->devices) {
        if (!d.selected) continue;
        for (auto& light : d.lights) {
            HRESULT hr =
                Invoke(light.get(), L"Color", DISPATCH_PROPERTYPUT, {MakeUI4(auraColor)}, nullptr);
            if (FAILED(hr)) {
                LUMA_WARN("Aura: set Color failed on \"%s\" (hr=0x%08lX)",
                          Narrow(d.info.name).c_str(), static_cast<unsigned long>(hr));
                return false;
            }
        }
        HRESULT hr = Invoke(d.device.get(), L"Apply", DISPATCH_METHOD, {}, nullptr);
        if (FAILED(hr)) {
            LUMA_WARN("Aura: Apply failed on \"%s\" (hr=0x%08lX)", Narrow(d.info.name).c_str(),
                      static_cast<unsigned long>(hr));
            return false;
        }
    }
    return true;
}

}  // namespace luma
