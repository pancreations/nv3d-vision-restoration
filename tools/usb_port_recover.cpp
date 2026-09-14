#include <windows.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>
#include <usbiodef.h>
#include <usbioctl.h>
#include <setupapi.h>
#include <shellapi.h>
#include <cwchar>
#include <vector>

namespace {
int cyclePort(const wchar_t* instanceId) {
    DEVINST device{};
    std::vector<wchar_t> id(instanceId, instanceId + std::wcslen(instanceId) + 1);
    if (CM_Locate_DevNodeW(&device, id.data(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return 10;

    DEVINST parent{};
    if (CM_Get_Parent(&parent, device, 0) != CR_SUCCESS) return 11;
    wchar_t parentId[MAX_DEVICE_ID_LEN]{};
    if (CM_Get_Device_IDW(parent, parentId, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) return 12;

    DEVPROPTYPE type{};
    ULONG size = sizeof(ULONG), port = 0;
    if (CM_Get_DevNode_PropertyW(device, &DEVPKEY_Device_Address, &type,
                                reinterpret_cast<PBYTE>(&port), &size, 0) != CR_SUCCESS ||
        type != DEVPROP_TYPE_UINT32 || port == 0) return 13;

    HDEVINFO hubs = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_USB_HUB, nullptr, nullptr,
                                         DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hubs == INVALID_HANDLE_VALUE) return 14;
    int result = 15;
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA iface{sizeof(iface)};
        if (!SetupDiEnumDeviceInterfaces(hubs, nullptr, &GUID_DEVINTERFACE_USB_HUB, index, &iface)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) break;
            continue;
        }
        DWORD bytes = 0;
        SetupDiGetDeviceInterfaceDetailW(hubs, &iface, nullptr, 0, &bytes, nullptr);
        if (!bytes) continue;
        std::vector<BYTE> storage(bytes);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
        detail->cbSize = sizeof(*detail);
        SP_DEVINFO_DATA hub{sizeof(hub)};
        if (!SetupDiGetDeviceInterfaceDetailW(hubs, &iface, detail, bytes, nullptr, &hub)) continue;
        wchar_t hubId[MAX_DEVICE_ID_LEN]{};
        if (!SetupDiGetDeviceInstanceIdW(hubs, &hub, hubId, MAX_DEVICE_ID_LEN, nullptr) ||
            _wcsicmp(hubId, parentId) != 0) continue;

        HANDLE handle = CreateFileW(detail->DevicePath, GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE) { result = 16; break; }
        USB_CYCLE_PORT_PARAMS params{};
        params.ConnectionIndex = port;
        DWORD returned = 0;
        BOOL ok = DeviceIoControl(handle, IOCTL_USB_HUB_CYCLE_PORT,
                                  &params, sizeof(params), &params, sizeof(params), &returned, nullptr);
        CloseHandle(handle);
        result = ok ? 0 : 17;
        break;
    }
    SetupDiDestroyDeviceInfoList(hubs);
    return result;
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc != 2) {
        if (argv) LocalFree(argv);
        return 2;
    }
    int result = cyclePort(argv[1]);
    LocalFree(argv);
    return result;
}
