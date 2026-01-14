#include <Windows.h>

#include <TlHelp32.h>
#include <iostream>
#include <string>
#include <vector>

DWORD FindProcess(const wchar_t *name) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE)
    return 0;

  PROCESSENTRY32W pe = {sizeof(pe)};
  DWORD pid = 0;

  if (Process32FirstW(snap, &pe)) {
    do {
      if (_wcsicmp(pe.szExeFile, name) == 0) {
        pid = pe.th32ProcessID;
        break;
      }
    } while (Process32NextW(snap, &pe));
  }

  CloseHandle(snap);
  return pid;
}

HMODULE FindModule(DWORD pid, const wchar_t *moduleName) {
  HANDLE snap =
      CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
  if (snap == INVALID_HANDLE_VALUE)
    return nullptr;

  MODULEENTRY32W me = {sizeof(me)};
  HMODULE result = nullptr;

  if (Module32FirstW(snap, &me)) {
    do {
      if (_wcsicmp(me.szModule, moduleName) == 0) {
        result = me.hModule;
        break;
      }
    } while (Module32NextW(snap, &me));
  }

  CloseHandle(snap);
  return result;
}

bool Unload(DWORD pid, HMODULE hModule) {
  HANDLE proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
  if (!proc) {
    std::wcerr << L"Failed to open process. Run as Administrator?\n";
    return false;
  }

  HANDLE thread =
      CreateRemoteThread(proc, nullptr, 0,
                         (LPTHREAD_START_ROUTINE)GetProcAddress(
                             GetModuleHandleW(L"kernel32.dll"), "FreeLibrary"),
                         hModule, 0, nullptr);

  if (!thread) {
    std::wcerr << L"Failed to create remote thread\n";
    CloseHandle(proc);
    return false;
  }

  WaitForSingleObject(thread, 5000);

  DWORD exitCode = 0;
  GetExitCodeThread(thread, &exitCode);

  CloseHandle(thread);
  CloseHandle(proc);

  return exitCode != 0;
}

int wmain(int argc, wchar_t *argv[]) {
  std::wcout << L"=== Dreadmyst Tracker Unloader ===\n\n";

  std::wcout << L"Looking for Dreadmyst.exe...\n";
  DWORD pid = FindProcess(L"Dreadmyst.exe");

  if (!pid) {
    std::wcerr << L"ERROR: Dreadmyst.exe not found!\n";
    return 1;
  }

  std::wcout << L"Found! PID: " << pid << L"\n";

  // List of DLLs to try unloading
  std::vector<std::wstring> dlls = {
      L"DreadmystTracker.dll",  L"DreadmystTracker1.dll",
      L"DreadmystTracker2.dll", L"DreadmystTracker3.dll",
      L"DreadmystTracker4.dll", L"DreadmystTracker5.dll",
      L"DreadmystTracker6.dll"};

  bool anyUnloaded = false;

  for (const auto &dllName : dlls) {
    HMODULE hModule = FindModule(pid, dllName.c_str());
    if (hModule) {
      std::wcout << L"Found " << dllName << L". Unloading...\n";
      if (Unload(pid, hModule)) {
        std::wcout << L"SUCCESS! " << dllName << L" unloaded!\n";
        anyUnloaded = true;
      } else {
        std::wcerr << L"FAILED to unload " << dllName << L"\n";
      }
    }
  }

  if (!anyUnloaded) {
    std::wcout << L"No Dreadmyst Tracker DLLs were found injected.\n";
  } else {
    std::wcout << L"\nCleanup complete.\n";
  }

  return 0;
}
