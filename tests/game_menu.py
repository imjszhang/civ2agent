#!/usr/bin/env python3
"""Test setup only: list civ2's menu bar, or post the menu item whose caption
contains the given text, as a click on it would.

python game_menu.py            # list every item with its id and state
python game_menu.py 离开游戏    # e.g. quit to the main menu after a live test

Not part of the agent interface; live checks use it to start and end a match.
"""

import ctypes
import subprocess
import sys
from ctypes import wintypes

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")

user32 = ctypes.windll.user32
user32.GetMenu.restype = wintypes.HMENU
user32.GetMenu.argtypes = [wintypes.HWND]
user32.GetSubMenu.restype = wintypes.HMENU
user32.GetSubMenu.argtypes = [wintypes.HMENU, ctypes.c_int]
user32.GetMenuItemCount.argtypes = [wintypes.HMENU]
user32.GetMenuItemID.argtypes = [wintypes.HMENU, ctypes.c_int]
user32.GetMenuItemID.restype = wintypes.UINT
user32.GetMenuStringW.argtypes = [wintypes.HMENU, wintypes.UINT, wintypes.LPWSTR, ctypes.c_int, wintypes.UINT]
user32.GetMenuState.argtypes = [wintypes.HMENU, wintypes.UINT, wintypes.UINT]
user32.GetMenuState.restype = wintypes.UINT
user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
MF_BYPOSITION, MF_GRAYED, MF_DISABLED, WM_COMMAND = 0x400, 0x1, 0x2, 0x0111


def main_window():
    out = subprocess.check_output(["tasklist", "/FI", "IMAGENAME eq civ2.exe", "/FO", "CSV", "/NH"], text=True)
    if "civ2.exe" not in out.lower():
        raise SystemExit("civ2.exe is not running")
    pid = int(out.split(",")[1].strip('"'))
    best = [None, 0]

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def visit(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd) and user32.GetMenu(hwnd):
            rect = wintypes.RECT()
            user32.GetWindowRect(hwnd, ctypes.byref(rect))
            area = (rect.right - rect.left) * (rect.bottom - rect.top)
            if area > best[1]:
                best[:] = [hwnd, area]
        return True

    user32.EnumWindows(visit, 0)
    if not best[0]:
        raise SystemExit("civ2 main window not found")
    return best[0]


def main(argv):
    hwnd = main_window()
    bar = user32.GetMenu(hwnd)
    want = argv[0] if argv else None
    for i in range(user32.GetMenuItemCount(bar)):
        sub = user32.GetSubMenu(bar, i)
        if not sub:
            continue
        for j in range(user32.GetMenuItemCount(sub)):
            buf = ctypes.create_unicode_buffer(128)
            if not user32.GetMenuStringW(sub, j, buf, 128, MF_BYPOSITION):
                continue
            item = user32.GetMenuItemID(sub, j)
            grey = bool(user32.GetMenuState(sub, j, MF_BYPOSITION) & (MF_GRAYED | MF_DISABLED))
            if want is None:
                print(f"menu {i} item {j} id={item}{' grey' if grey else ''} {buf.value!r}")
            elif want in buf.value:
                if grey:
                    raise SystemExit(f"{buf.value!r} is greyed out")
                print(f"posting {buf.value!r} (id {item})")
                user32.PostMessageW(hwnd, WM_COMMAND, item, 0)
                return 0
    if want:
        raise SystemExit(f"no menu item containing {want!r}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
