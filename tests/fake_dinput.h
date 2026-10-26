// Fake DirectInput 8 objects for differential tests (compare_input, compare_window): each side of a
// test gets its own World, whose devices return the World's scripted keyboard and mouse states
// and log every call they get.
#pragma once

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "../src/dx/dinput_types.h"

namespace {

// One side's fake DirectInput: its devices, the scripted states and the log of every call.
struct World {
    std::vector<std::string> log;
    std::vector<IDirectInputDevice8A *> devices;
    uint8_t keys[256] = {};
    DIMOUSESTATE2 mouse = {};
    DWORD axes = 3;
    int failStates = 0;                 // GetDeviceState calls to fail with DIERR_INPUTLOST
    int lostAcquires = 0;               // Acquire calls to fail with DIERR_INPUTLOST
    int failCreate = 0;                 // CreateDevice calls to fail
    int failFormat = 0;                 // SetDataFormat calls to fail
    int failLevel = 0;                  // SetCooperativeLevel calls to fail

    void Log(const char *fmt, ...)
    {
        char text[160];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(text, sizeof text, fmt, args);
        va_end(args);
        log.push_back(text);
    }
};

class FakeDevice : public IDirectInputDevice8A {
public:
    FakeDevice(World *world, bool mouse, int id) : w(world), isMouse(mouse), id(id) {}

    HRESULT __stdcall QueryInterface(REFIID, LPVOID *) override { return Unexpected("QueryInterface"); }
    ULONG __stdcall AddRef() override
    {
        Log("AddRef");
        return ++refs;
    }
    ULONG __stdcall Release() override
    {
        Log("Release");
        return --refs;
    }
    HRESULT __stdcall GetCapabilities(LPDIDEVCAPS caps) override
    {
        Log("GetCapabilities %lu", caps->dwSize);
        const DWORD size = caps->dwSize;
        std::memset(caps, 0, size);
        caps->dwSize = size;
        caps->dwAxes = isMouse ? w->axes : 0;
        caps->dwButtons = isMouse ? 5 : 128;
        return DI_OK;
    }
    HRESULT __stdcall EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA, LPVOID, DWORD) override
    {
        return Unexpected("EnumObjects");
    }
    HRESULT __stdcall GetProperty(REFGUID property, LPDIPROPHEADER header) override
    {
        // (DIPROP_KEYNAME is a small integer posing as a GUID reference: logged as that number.)
        Log("GetProperty %u %lu %lu %lu %lu", static_cast<unsigned>(reinterpret_cast<uintptr_t>(&property)),
            header->dwSize, header->dwHeaderSize, header->dwObj, header->dwHow);
        if (header->dwObj % 7 == 0)
            return DIERR_NOTFOUND;
        auto *name = reinterpret_cast<DIPROPSTRING *>(header);
        std::swprintf(name->wsz, MAX_PATH, L"Key %lu", header->dwObj);
        return DI_OK;
    }
    HRESULT __stdcall SetProperty(REFGUID, LPCDIPROPHEADER) override { return Unexpected("SetProperty"); }
    HRESULT __stdcall Acquire() override
    {
        Log("Acquire");
        if (w->lostAcquires > 0) {
            --w->lostAcquires;
            return DIERR_INPUTLOST;
        }
        return DI_OK;
    }
    HRESULT __stdcall Unacquire() override
    {
        Log("Unacquire");
        return DI_OK;
    }
    HRESULT __stdcall GetDeviceState(DWORD size, LPVOID data) override
    {
        Log("GetDeviceState %lu", size);
        if (w->failStates > 0) {
            --w->failStates;
            return DIERR_INPUTLOST;
        }
        if (isMouse)
            std::memcpy(data, &w->mouse, size < sizeof w->mouse ? size : sizeof w->mouse);
        else
            std::memcpy(data, w->keys, size < sizeof w->keys ? size : sizeof w->keys);
        return DI_OK;
    }
    HRESULT __stdcall GetDeviceData(DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD) override
    {
        return Unexpected("GetDeviceData");
    }
    HRESULT __stdcall SetDataFormat(LPCDIDATAFORMAT format) override
    {
        // (Each side passes its own copy of c_dfDIKeyboard / c_dfDIMouse2: compared by content.)
        Log("SetDataFormat %lu %lu %lu %lu %lu", format->dwSize, format->dwObjSize, format->dwFlags, format->dwDataSize,
            format->dwNumObjs);
        if (w->failFormat > 0) {
            --w->failFormat;
            return DIERR_INVALIDPARAM;
        }
        return DI_OK;
    }
    HRESULT __stdcall SetEventNotification(HANDLE) override { return Unexpected("SetEventNotification"); }
    HRESULT __stdcall SetCooperativeLevel(HWND window, DWORD flags) override
    {
        Log("SetCooperativeLevel %p %lx", static_cast<void *>(window), flags);
        if (w->failLevel > 0) {
            --w->failLevel;
            return DIERR_INVALIDPARAM;
        }
        return DI_OK;
    }
    HRESULT __stdcall GetObjectInfo(LPDIDEVICEOBJECTINSTANCEA, DWORD, DWORD) override
    {
        return Unexpected("GetObjectInfo");
    }
    HRESULT __stdcall GetDeviceInfo(LPDIDEVICEINSTANCEA) override { return Unexpected("GetDeviceInfo"); }
    HRESULT __stdcall RunControlPanel(HWND, DWORD) override { return Unexpected("RunControlPanel"); }
    HRESULT __stdcall Initialize(HINSTANCE, DWORD, REFGUID) override { return Unexpected("Initialize"); }
    HRESULT __stdcall CreateEffect(REFGUID, LPCDIEFFECT, LPDIRECTINPUTEFFECT *, LPUNKNOWN) override
    {
        return Unexpected("CreateEffect");
    }
    HRESULT __stdcall EnumEffects(LPDIENUMEFFECTSCALLBACKA, LPVOID, DWORD) override
    {
        return Unexpected("EnumEffects");
    }
    HRESULT __stdcall GetEffectInfo(LPDIEFFECTINFOA, REFGUID) override { return Unexpected("GetEffectInfo"); }
    HRESULT __stdcall GetForceFeedbackState(LPDWORD) override { return Unexpected("GetForceFeedbackState"); }
    HRESULT __stdcall SendForceFeedbackCommand(DWORD) override { return Unexpected("SendForceFeedbackCommand"); }
    HRESULT __stdcall EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK, LPVOID, DWORD) override
    {
        return Unexpected("EnumCreatedEffectObjects");
    }
    HRESULT __stdcall Escape(LPDIEFFESCAPE) override { return Unexpected("Escape"); }
    HRESULT __stdcall Poll() override { return Unexpected("Poll"); }
    HRESULT __stdcall SendDeviceData(DWORD, LPCDIDEVICEOBJECTDATA, LPDWORD, DWORD) override
    {
        return Unexpected("SendDeviceData");
    }
    HRESULT __stdcall EnumEffectsInFile(LPCSTR, LPDIENUMEFFECTSINFILECALLBACK, LPVOID, DWORD) override
    {
        return Unexpected("EnumEffectsInFile");
    }
    HRESULT __stdcall WriteEffectToFile(LPCSTR, DWORD, LPDIFILEEFFECT, DWORD) override
    {
        return Unexpected("WriteEffectToFile");
    }
    HRESULT __stdcall BuildActionMap(LPDIACTIONFORMATA, LPCSTR, DWORD) override { return Unexpected("BuildActionMap"); }
    HRESULT __stdcall SetActionMap(LPDIACTIONFORMATA, LPCSTR, DWORD) override { return Unexpected("SetActionMap"); }
    HRESULT __stdcall GetImageInfo(LPDIDEVICEIMAGEINFOHEADERA) override { return Unexpected("GetImageInfo"); }

private:
    void Log(const char *fmt, ...)
    {
        char text[160];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(text, sizeof text, fmt, args);
        va_end(args);
        w->Log("%c%d.%s", isMouse ? 'M' : 'K', id, text);
    }
    HRESULT Unexpected(const char *name)
    {
        Log("UNEXPECTED %s", name);
        return E_NOTIMPL;
    }

    World *w;
    bool isMouse;
    int id;
    ULONG refs = 1;
};

class FakeDInput : public IDirectInput8A {
public:
    explicit FakeDInput(World *world) : w(world) {}

    HRESULT __stdcall QueryInterface(REFIID, LPVOID *) override { return Unexpected("QueryInterface"); }
    ULONG __stdcall AddRef() override { return 1; }
    ULONG __stdcall Release() override
    {
        w->Log("DI.Release");
        return 0;
    }
    HRESULT __stdcall CreateDevice(REFGUID guid, LPDIRECTINPUTDEVICE8A *device, LPUNKNOWN outer) override
    {
        const bool mouse = IsEqualGUID(guid, GUID_SysMouse) != 0;
        const bool keyboard = IsEqualGUID(guid, GUID_SysKeyboard) != 0;
        w->Log("DI.CreateDevice %s %d", mouse ? "mouse" : keyboard ? "keyboard" : "?", outer != nullptr);
        if (w->failCreate > 0) {
            --w->failCreate;
            return DIERR_DEVICENOTREG;
        }
        auto *d = new FakeDevice(w, mouse, static_cast<int>(w->devices.size()));
        w->devices.push_back(d);
        *device = d;
        return DI_OK;
    }
    HRESULT __stdcall EnumDevices(DWORD, LPDIENUMDEVICESCALLBACKA, LPVOID, DWORD) override
    {
        return Unexpected("EnumDevices");
    }
    HRESULT __stdcall GetDeviceStatus(REFGUID) override { return Unexpected("GetDeviceStatus"); }
    HRESULT __stdcall RunControlPanel(HWND, DWORD) override { return Unexpected("RunControlPanel"); }
    HRESULT __stdcall Initialize(HINSTANCE, DWORD) override { return Unexpected("Initialize"); }
    HRESULT __stdcall FindDevice(REFGUID, LPCSTR, LPGUID) override { return Unexpected("FindDevice"); }
    HRESULT __stdcall EnumDevicesBySemantics(LPCSTR, LPDIACTIONFORMATA, LPDIENUMDEVICESBYSEMANTICSCBA, LPVOID,
                                             DWORD) override
    {
        return Unexpected("EnumDevicesBySemantics");
    }
    HRESULT __stdcall ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK, LPDICONFIGUREDEVICESPARAMSA, DWORD,
                                       LPVOID) override
    {
        return Unexpected("ConfigureDevices");
    }

private:
    HRESULT Unexpected(const char *name)
    {
        w->Log("DI.UNEXPECTED %s", name);
        return E_NOTIMPL;
    }

    World *w;
};

}  // namespace
