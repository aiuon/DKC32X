/* DKC32X Builder.exe: starts the bundled Python GUI (app\tools\gui.py) with pythonw.exe, passing on
   any file dropped onto the exe.  Build: x86_64-w64-mingw32-gcc -O2 -mwindows -municode launcher.c res.o -o "DKC32X Builder.exe" */
#include <windows.h>
#include <wchar.h>

int WINAPI wWinMain(HINSTANCE h, HINSTANCE p, PWSTR cmd, int show)
{
    static wchar_t dir[MAX_PATH], exe[MAX_PATH + 64], line[32768];
    DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    while (n && dir[n - 1] != L'\\' && dir[n - 1] != L'/') n--;
    dir[n] = 0;
    swprintf(exe, MAX_PATH + 64, L"%lsapp\\runtime\\python\\pythonw.exe", dir);
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(NULL, L"The app folder is missing. Unzip the whole DKC32X Builder folder (not just this .exe) and run it from there.",
                    L"DKC32X Builder", MB_ICONERROR);
        return 1;
    }
    /* the rest of our own command line (the dropped file, already quoted by Explorer when needed) */
    swprintf(line, 32768, L"\"%ls\" \"%lsapp\\tools\\gui.py\" %ls", exe, dir, cmd ? cmd : L"");
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, line, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        MessageBoxW(NULL, L"Could not start the bundled Python (app\\runtime\\python\\pythonw.exe).", L"DKC32X Builder", MB_ICONERROR);
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
