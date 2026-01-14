#include <Windows.h>

#include <TlHelp32.h>
#include <filesystem>
#include <iostream>
#include <string>

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

bool Inject(DWORD pid, const std::wstring &dllPath) {
  // 1. Open target process
  HANDLE proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
  if (!proc) {
    std::cerr << "Failed to open process. Run as Admin?" << std::endl;
    return false;
  }

  // 2. Allocate memory for DLL path
  size_t size = (dllPath.size() + 1) * sizeof(wchar_t);
  void *remoteMem =
      VirtualAllocEx(proc, nullptr, size, MEM_COMMIT, PAGE_READWRITE);
  if (!remoteMem) {
    std::cerr << "Failed to allocate remote memory" << std::endl;
    CloseHandle(proc);
    return false;
  }

  // 3. Write DLL path
  WriteProcessMemory(proc, remoteMem, dllPath.c_str(), size, nullptr);

  // 4. Create remote thread to LoadLibraryW
  HANDLE thread =
      CreateRemoteThread(proc, nullptr, 0,
                         (LPTHREAD_START_ROUTINE)GetProcAddress(
                             GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"),
                         remoteMem, 0, nullptr);

  if (!thread) {
    std::cerr << "Failed to create remote thread" << std::endl;
    VirtualFreeEx(proc, remoteMem, 0, MEM_RELEASE);
    CloseHandle(proc);
    return false;
  }

  WaitForSingleObject(thread, 5000);
  VirtualFreeEx(proc, remoteMem, 0, MEM_RELEASE);
  CloseHandle(thread);
  CloseHandle(proc);
  return true;
}

int wmain(int argc, wchar_t *argv[]) {
  std::wcout << L"=== Dreadmyst Tracker Injector ===\n\n";

  // Accept DLL path from command line, or use current directory
  std::filesystem::path dllPath;
  if (argc >= 2) {
    dllPath = argv[1];
  } else {
    std::filesystem::path currentPath = std::filesystem::current_path();
    dllPath = currentPath / L"DreadmystTracker.dll";
  }

  if (!std::filesystem::exists(dllPath)) {
    std::wcerr << L"ERROR: DreadmystTracker.dll not found!\n";
    std::wcerr << L"Expected: " << dllPath.wstring() << L"\n";
    return 1;
  }

  std::wcout << L"Looking for Dreadmyst.exe...\n";
  DWORD pid = FindProcess(L"Dreadmyst.exe");

  if (!pid) {
    std::wcerr << L"ERROR: Dreadmyst.exe not found. Start game first.\n";
    return 1;
  }

  std::wcout << L"Found Game PID: " << pid << L"\n";
  std::wcout << L"Injecting: " << dllPath.filename().wstring() << L"...\n";

  if (Inject(pid, dllPath.wstring())) {
    std::wcout << L"\n*** SUCCESS! DLL Injected! ***\n";
  } else {
    std::wcerr << L"\n*** FAILED to inject ***\n";
  }

  return 0;
}
