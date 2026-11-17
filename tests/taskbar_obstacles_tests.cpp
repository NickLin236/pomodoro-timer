// Test the actual private MSAA reader with deterministic COM objects. No
// Explorer window, tray registration, or notification service is touched.
#include "../src/platform/taskbar_obstacles.cpp"

#include <iostream>
#include <stdexcept>

using namespace pomodoro;

namespace {
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
    std::string(__func__) + ":" + std::to_string(__LINE__) + ": " #condition); } while (false)

VARIANT childId(LONG value) { VARIANT child{}; child.vt = VT_I4; child.lVal = value; return child; }

class FakeAccessible;
struct AccessibleElement {
    LONG role = ROLE_SYSTEM_PUSHBUTTON;
    LONG state = 0;
    RECT bounds{10, 0, 100, 40};
    HRESULT stateResult = S_OK;
    HRESULT locationResult = S_OK;
    FakeAccessible* object = nullptr;
    DWORD stateDelayMs = 0;
};

class FakeAccessible final : public IAccessible {
public:
    AccessibleElement self;
    std::vector<AccessibleElement> children;
    ULONG references = 1;
    unsigned forbiddenParentProperties = 0;

    FakeAccessible() { self.role = ROLE_SYSTEM_LIST; self.bounds = {0, 0, 1000, 40}; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** target) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { CHECK(references > 1); return --references; }
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* count) override { *count = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return DISP_E_UNKNOWNNAME; }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override { return DISP_E_MEMBERNOTFOUND; }
    HRESULT STDMETHODCALLTYPE get_accParent(IDispatch** parent) override { *parent = nullptr; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accChildCount(LONG* count) override { *count = static_cast<LONG>(children.size()); return S_OK; }
    HRESULT STDMETHODCALLTYPE get_accChild(VARIANT child, IDispatch** result) override {
        *result = nullptr;
        const auto element = find(child);
        if (!element) return E_INVALIDARG;
        if (!element->object) return S_FALSE;
        *result = element->object;
        element->object->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accRole(VARIANT child, VARIANT* role) override {
        const auto element = properties(child);
        if (!element) return E_INVALIDARG;
        VariantInit(role); role->vt = VT_I4; role->lVal = element->role; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accState(VARIANT child, VARIANT* state) override {
        const auto element = properties(child);
        if (!element) return E_INVALIDARG;
        if (element->stateDelayMs) Sleep(element->stateDelayMs);
        if (FAILED(element->stateResult)) return element->stateResult;
        VariantInit(state); state->vt = VT_I4; state->lVal = element->state; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accLocation(LONG* x, LONG* y, LONG* width, LONG* height, VARIANT child) override {
        const auto element = properties(child);
        if (!element) return E_INVALIDARG;
        if (FAILED(element->locationResult)) return element->locationResult;
        *x = element->bounds.left; *y = element->bounds.top;
        *width = element->bounds.right - element->bounds.left;
        *height = element->bounds.bottom - element->bounds.top;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accName(VARIANT, BSTR* text) override { *text = nullptr; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accValue(VARIANT, BSTR* text) override { *text = nullptr; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accDescription(VARIANT, BSTR* text) override { *text = nullptr; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accHelp(VARIANT, BSTR* text) override { *text = nullptr; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR* text, VARIANT, LONG* topic) override { *text = nullptr; *topic = 0; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accKeyboardShortcut(VARIANT, BSTR* text) override { *text = nullptr; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT* child) override { VariantInit(child); return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accSelection(VARIANT* child) override { VariantInit(child); return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT, BSTR* text) override { *text = nullptr; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE accSelect(LONG, VARIANT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE accNavigate(LONG, VARIANT, VARIANT* result) override { VariantInit(result); return S_FALSE; }
    HRESULT STDMETHODCALLTYPE accHitTest(LONG, LONG, VARIANT* result) override { *result = childId(CHILDID_SELF); return S_OK; }
    HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE put_accName(VARIANT, BSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE put_accValue(VARIANT, BSTR) override { return E_NOTIMPL; }
private:
    AccessibleElement* find(VARIANT child) {
        if (child.vt != VT_I4 || child.lVal < 0 || child.lVal > static_cast<LONG>(children.size())) return nullptr;
        return child.lVal == CHILDID_SELF ? &self : &children[static_cast<std::size_t>(child.lVal - 1)];
    }
    AccessibleElement* properties(VARIANT child) {
        const auto element = find(child);
        // The parent of a full object is permitted not to provide any of its
        // properties. The reader must first resolve the child object itself.
        if (element && element->object) { ++forbiddenParentProperties; return nullptr; }
        return element;
    }
};

class FakeEnumerator final : public IEnumVARIANT {
public:
    explicit FakeEnumerator(FakeAccessible& parent, std::size_t position = 0) : parent_(parent), position_(position) { parent_.AddRef(); }
    ~FakeEnumerator() { parent_.Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** target) override {
        *target = nullptr;
        if (iid == IID_IUnknown || iid == IID_IEnumVARIANT) { *target = static_cast<IEnumVARIANT*>(this); AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto left = --references_; if (!left) delete this; return left; }
    HRESULT STDMETHODCALLTYPE Next(ULONG count, VARIANT* values, ULONG* fetched) override {
        ULONG read = 0;
        while (read < count && position_ < parent_.children.size())
            values[read++] = childId(static_cast<LONG>(++position_));
        if (fetched) *fetched = read;
        return read == count ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Skip(ULONG count) override {
        const auto available = parent_.children.size() - position_;
        position_ += (std::min)(available, static_cast<std::size_t>(count));
        return available >= count ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Reset() override { position_ = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE Clone(IEnumVARIANT** result) override { *result = new FakeEnumerator(parent_, position_); return S_OK; }
private:
    FakeAccessible& parent_;
    std::size_t position_;
    ULONG references_ = 1;
};

HRESULT FakeAccessible::QueryInterface(REFIID iid, void** target) {
    *target = nullptr;
    if (iid == IID_IUnknown || iid == IID_IDispatch || iid == IID_IAccessible) {
        *target = static_cast<IAccessible*>(this); AddRef(); return S_OK;
    }
    if (iid == IID_IEnumVARIANT) { *target = new FakeEnumerator(*this); return S_OK; }
    return E_NOINTERFACE;
}

class PrivateHost {
public:
    PrivateHost() {
        using Set = HANDLE (WINAPI*)(HANDLE);
        set_ = loadFunction<Set>(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext");
        CHECK(set_);
        previous_ = set_(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)));
        CHECK(previous_);
        window = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Private geometry test", WS_POPUP,
            0, 0, 1000, 40, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        CHECK(window);
    }
    ~PrivateHost() { if (window) DestroyWindow(window); if (previous_) set_(previous_); }
    HWND window = nullptr;
private:
    using Set = HANDLE (WINAPI*)(HANDLE);
    Set set_ = nullptr;
    HANDLE previous_ = nullptr;
};

struct ComLifetime {
    HRESULT status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComLifetime() { if (SUCCEEDED(status)) CoUninitialize(); }
};

bool read(FakeAccessible& object, HWND host, std::vector<RECT>& occupied,
          AccessibleReadError& error, VARIANT child = childId(CHILDID_SELF)) {
    std::size_t remaining = maximumAccessibleNodes;
    const RECT strip{0, 0, 1000, 40};
    return inspectAccessible(&object, child, host, strip, occupied, remaining, 0,
        child.lVal == CHILDID_SELF, error);
}

void simpleElementsAndTheirUnknownRolesRemainMeasurable(HWND host) {
    FakeAccessible root;
    root.children.push_back({ROLE_SYSTEM_PROGRESSBAR, 0, {10, 0, 180, 40}, S_OK, S_OK, nullptr});
    std::vector<RECT> occupied;
    AccessibleReadError error;
    CHECK(read(root, host, occupied, error));
    CHECK(occupied.size() == 1 && occupied.front().left == 10 && occupied.front().right == 180);
    CHECK(root.references == 1);
}

void fullChildrenSupplyTheirOwnProperties(HWND host) {
    FakeAccessible root, button;
    button.self.role = ROLE_SYSTEM_PUSHBUTTON;
    button.self.bounds = {20, 0, 200, 40};
    button.children.push_back({ROLE_SYSTEM_STATICTEXT, 0, {30, 5, 80, 30}, S_OK, S_OK, nullptr});
    root.children.push_back({ROLE_SYSTEM_CLIENT, 0, {0, 0, 1000, 40}, S_OK, S_OK, &button});
    std::vector<RECT> occupied;
    AccessibleReadError error;
    CHECK(read(root, host, occupied, error, childId(1)));
    CHECK(root.forbiddenParentProperties == 0);
    CHECK(occupied.size() == 1 && occupied.front().right == 200);
    CHECK(button.references == 1 && root.references == 1);
}

void textAndGraphicRootContainersDoNotFillTheTaskbar(HWND host) {
    for (const LONG role : {ROLE_SYSTEM_STATICTEXT, ROLE_SYSTEM_GRAPHIC}) {
        FakeAccessible root;
        root.self.role = role;
        root.children.push_back({ROLE_SYSTEM_PUSHBUTTON, 0, {30, 0, 160, 40}, S_OK, S_OK, nullptr});
        std::vector<RECT> occupied;
        AccessibleReadError error;
        CHECK(read(root, host, occupied, error));
        CHECK(occupied.size() == 1 && occupied.front().left == 30 && occupied.front().right == 160);
    }
}

void unavailablePropertiesUseGeometryAndUnknownGeometryStillFails(HWND host) {
    FakeAccessible root;
    root.children.push_back({ROLE_SYSTEM_PUSHBUTTON, 0, {40, 0, 170, 40}, E_ACCESSDENIED, S_OK, nullptr});
    std::vector<RECT> occupied;
    AccessibleReadError error;
    CHECK(read(root, host, occupied, error));
    CHECK(occupied.size() == 1 && occupied.front().right == 170);
    root.children.front().locationResult = E_FAIL;
    occupied.clear();
    CHECK(!read(root, host, occupied, error));
    CHECK(error.status == E_FAIL && std::wstring(error.stage) == L"IAccessible::accLocation");
    root.children.front().stateResult = S_OK;
    root.children.front().state = STATE_SYSTEM_INVISIBLE;
    CHECK(read(root, host, occupied, error) && occupied.empty());
}

void slowProviderDoesNotDiscardAValidMeasurement(HWND host) {
    FakeAccessible root;
    root.self.stateDelayMs = 600;
    root.children.push_back({ROLE_SYSTEM_PUSHBUTTON, 0, {20, 0, 180, 40}, S_OK, S_OK, nullptr});
    std::vector<RECT> occupied;
    AccessibleReadError error;
    CHECK(read(root, host, occupied, error));
    CHECK(occupied.size() == 1 && occupied.front().right == 180);
}

void perMonitorBoundsDoNotNeedEdgePointConversion(HWND host) {
    const RECT clip{0, 0, 1000, 40};
    RECT actual{};
    CHECK(physicalBoundsInHost(host, {10, -5, 1000, 45}, clip, actual) == S_OK);
    const RECT expected{10, 0, 1000, 40};
    CHECK(EqualRect(&actual, &expected));
    CHECK(physicalBoundsInHost(host, {1000, 0, 1100, 40}, clip, actual) == S_FALSE);
}

void reparentedPopupClipsAgainstItsStructuralParent(HWND host) {
    ShowWindow(host, SW_SHOWNOACTIVATE);
    HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Hidden popup owner", WS_POPUP,
        -32000, -32000, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    CHECK(owner);
    HWND popup = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Embedded monitor", WS_POPUP,
        0, 0, 100, 40, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    CHECK(popup);
    SetParent(popup, host);
    CHECK(GetAncestor(popup, GA_PARENT) == host);
    CHECK(GetParent(popup) != host);
    CHECK(GetWindowLongPtrW(popup, GWL_STYLE) & WS_POPUP);
    CHECK(!(GetWindowLongPtrW(owner, GWL_STYLE) & WS_VISIBLE));
    CHECK(SetWindowPos(popup, nullptr, 700, -4, 400, 48, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW));
    RECT strip{}, actual{};
    CHECK(clientScreenBounds(host, strip));
    CHECK(clippedDescendantBounds(popup, host, strip, actual));
    const RECT expected{strip.left + 700, strip.top, strip.right, strip.bottom};
    CHECK(EqualRect(&actual, &expected));
    DestroyWindow(popup);
    DestroyWindow(owner);
    ShowWindow(host, SW_HIDE);
}
} // namespace

int main() {
    try {
        ComLifetime com;
        CHECK(SUCCEEDED(com.status));
        PrivateHost host;
        simpleElementsAndTheirUnknownRolesRemainMeasurable(host.window);
        fullChildrenSupplyTheirOwnProperties(host.window);
        textAndGraphicRootContainersDoNotFillTheTaskbar(host.window);
        unavailablePropertiesUseGeometryAndUnknownGeometryStillFails(host.window);
        perMonitorBoundsDoNotNeedEdgePointConversion(host.window);
        reparentedPopupClipsAgainstItsStructuralParent(host.window);
        slowProviderDoesNotDiscardAValidMeasurement(host.window);
        std::cout << "7 taskbar accessibility and window geometry scenarios passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
