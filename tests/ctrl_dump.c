#include <windows.h>
#include <stdio.h>

static HWND g_parent;

static BOOL CALLBACK Child(HWND hwnd, LPARAM lp) {
    RECT r;
    LONG_PTR data;
    unsigned char buf[8];
    SIZE_T got = 0;
    GetWindowRect(hwnd, &r);
    data = GetWindowLongA(hwnd, 0);
    HANDLE proc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, GetCurrentProcessId());
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    proc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (proc && ReadProcessMemory(proc, (void*)(uintptr_t)data, buf, 8, &got)) {
        unsigned type = *(unsigned*)buf;
        unsigned code = *(unsigned*)(buf + 4);
        printf("top=%d left=%d w=%d h=%d type=%u code=%u\n", r.top, r.left, r.right - r.left, r.bottom - r.top, type, code);
        if (proc) CloseHandle(proc);
    } else {
        printf("top=%d unreadable %p err=%lu\n", r.top, (void*)(uintptr_t)data, GetLastError());
        if (proc) CloseHandle(proc);
    }
    return TRUE;
}

static BOOL CALLBACK Top(HWND hwnd, LPARAM lp) {
    char title[160];
    if (!IsWindowVisible(hwnd)) return TRUE;
    GetWindowTextA(hwnd, title, 160);
    RECT wr;
    GetWindowRect(hwnd, &wr);
    int w = wr.right - wr.left;
    if (w < 280 || w > 700) return TRUE;
    printf("win %s %dx%d\n", title, w, wr.bottom - wr.top);
    g_parent = hwnd;
    return TRUE;
}

int main(void) {
    EnumWindows(Top, 0);
    if (!g_parent) {
        printf("no dialog\n");
        return 1;
    }
    printf("parent %p\n", g_parent);
    EnumChildWindows(g_parent, Child, 0);
    return 0;
}
