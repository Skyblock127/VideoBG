#include "common.h"
#include <shellapi.h>
#include <shlobj.h>
#include <exdisp.h>
#include <shldisp.h>
#include <cwchar>

// When VideoBG is started from inside an MSIX-packaged app (for example a packaged app that
// runs a terminal), Windows can silently redirect its %APPDATA% writes into that app's private
// storage (...\Packages\<app>\LocalCache). Settings, crops and the lock screen backup would then
// go to a hidden copy that the normal VideoBG (Start menu / startup) never sees. Detect that by
// creating a file and asking where it really went.
static bool AppDataRedirected() {
    std::wstring probe = AppDataDir() + L"\\.where-" + std::to_wstring(GetCurrentProcessId());
    HANDLE f = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    wchar_t real[1024];
    DWORD n = GetFinalPathNameByHandleW(f, real, (DWORD)std::size(real), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(f);
    if (n == 0 || n >= std::size(real)) return false;
    const wchar_t* p = wcsncmp(real, L"\\\\?\\", 4) == 0 ? real + 4 : real;
    bool redirected = _wcsicmp(p, probe.c_str()) != 0 && wcsstr(p, L"\\LocalCache\\") != nullptr;
    if (redirected) Log(L"started inside a packaged app's container (AppData goes to %ls); relaunching via Explorer", p);
    return redirected;
}

// Starts VideoBG again through Explorer's shell (so it runs as a normal desktop app, outside the
// container). See "How can I launch an unelevated process from my elevated process" (Raymond Chen).
static bool RelaunchViaExplorer(const wchar_t* args) {
    static const GUID kSID_STopLevelBrowser = {0x4c96be40, 0x915c, 0x11cf, {0x99, 0xd3, 0x00, 0xaa, 0x00, 0x4a, 0xe8, 0x37}};
    bool ok = false;
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellWindows* windows = nullptr;
    IDispatch* desktop = nullptr;
    IServiceProvider* sp = nullptr;
    IShellBrowser* browser = nullptr;
    IShellView* view = nullptr;
    IDispatch* bg = nullptr;
    IShellFolderViewDual* folder = nullptr;
    IDispatch* app = nullptr;
    IShellDispatch2* shell = nullptr;
    VARIANT loc, empty;
    VariantInit(&loc);
    VariantInit(&empty);
    loc.vt = VT_I4;
    loc.lVal = CSIDL_DESKTOP;
    long hwnd = 0;
    HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_IShellWindows, (void**)&windows);
    if (SUCCEEDED(hr)) hr = windows->FindWindowSW(&loc, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &desktop);
    if (SUCCEEDED(hr) && desktop) hr = desktop->QueryInterface(IID_IServiceProvider, (void**)&sp);
    else if (SUCCEEDED(hr)) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = sp->QueryService(kSID_STopLevelBrowser, IID_IShellBrowser, (void**)&browser);
    if (SUCCEEDED(hr)) hr = browser->QueryActiveShellView(&view);
    if (SUCCEEDED(hr)) hr = view->GetItemObject(SVGIO_BACKGROUND, IID_IDispatch, (void**)&bg);
    if (SUCCEEDED(hr)) hr = bg->QueryInterface(IID_IShellFolderViewDual, (void**)&folder);
    if (SUCCEEDED(hr)) hr = folder->get_Application(&app);
    if (SUCCEEDED(hr)) hr = app->QueryInterface(IID_IShellDispatch2, (void**)&shell);
    if (SUCCEEDED(hr)) {
        std::wstring exe = ExePath(), dir = exe.substr(0, exe.find_last_of(L'\\'));
        BSTR file = SysAllocString(exe.c_str());
        VARIANT vArgs, vDir, vOp, vShow;
        VariantInit(&vArgs);
        VariantInit(&vDir);
        VariantInit(&vOp);
        VariantInit(&vShow);
        vArgs.vt = VT_BSTR;
        vArgs.bstrVal = SysAllocString(args);
        vDir.vt = VT_BSTR;
        vDir.bstrVal = SysAllocString(dir.c_str());
        vShow.vt = VT_I4;
        vShow.lVal = SW_SHOWNORMAL;
        hr = shell->ShellExecute(file, vArgs, vDir, vOp, vShow);
        ok = SUCCEEDED(hr);
        SysFreeString(file);
        VariantClear(&vArgs);
        VariantClear(&vDir);
    }
    IUnknown* all[] = {shell, app, folder, bg, view, browser, sp, desktop, windows};
    for (IUnknown* p : all)
        if (p) p->Release();
    if (SUCCEEDED(init)) CoUninitialize();
    if (!ok) Log(L"relaunch via Explorer failed (0x%08lx); running here anyway", (unsigned long)hr);
    return ok;
}

// Command line:
//   VideoBG.exe            start (or open settings of the running instance)
//   VideoBG.exe --startup  start silently in the tray (used by the Windows startup entry)
//   VideoBG.exe --toggle   toggle the wallpaper of the running instance
//   VideoBG.exe --exit     close the running instance
//   VideoBG.exe --render   internal: the video renderer process
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    auto has = [&](const wchar_t* a) {
        for (int i = 1; i < argc; i++)
            if (!_wcsicmp(argv[i], a)) return true;
        return false;
    };

    if (argc >= 3 && !wcscmp(argv[1], L"--render")) {
        HWND host = (HWND)(UINT_PTR)_wcstoui64(argv[2], nullptr, 10);
        LocalFree(argv);
        return RendererMain(inst, host);
    }

    bool startup = has(L"--startup"), toggle = has(L"--toggle"), exitCmd = has(L"--exit"), outside = has(L"--outside");
    bool ownPictures = has(L"--restore-pictures");
    LocalFree(argv);

    // Uninstalling: put the user's own lock screen and background back if VideoBG's frame is still
    // there (after a crash, VideoBG had no chance to).
    if (ownPictures) {
        LockScreen_Sync(nullptr, 0, LockScreenWant{});
        LockScreen_Wait(15000);
        return 0;
    }

    HWND existing = FindWindowW(HOST_CLASS, nullptr);
    if (exitCmd) {
        if (existing) {
            DWORD pid = 0;
            GetWindowThreadProcessId(existing, &pid);
            HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
            PostMessageW(existing, WM_VBG_EXIT, 0, 0);
            if (p) { WaitForSingleObject(p, 5000); CloseHandle(p); }
        }
        return 0;
    }

    // --outside marks the relaunched copy, so a detection mistake can never loop.
    if (!existing && !outside && AppDataRedirected() &&
        RelaunchViaExplorer(startup || toggle ? L"--startup --outside" : L"--outside"))
        return 0;

    HANDLE mutex = CreateMutexW(nullptr, TRUE, HOST_MUTEX);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        for (int i = 0; i < 20 && !existing; i++) {  // the other instance may still be starting
            Sleep(100);
            existing = FindWindowW(HOST_CLASS, nullptr);
        }
        if (existing) {
            DWORD pid = 0;
            GetWindowThreadProcessId(existing, &pid);
            AllowSetForegroundWindow(pid);
            if (toggle) PostMessageW(existing, WM_VBG_TOGGLE, 0, 0);
            else if (!startup) PostMessageW(existing, WM_VBG_SHOW, 0, 0);
        }
        return 0;
    }
    int r = HostMain(inst, !startup && !toggle);
    if (mutex) CloseHandle(mutex);
    return r;
}
