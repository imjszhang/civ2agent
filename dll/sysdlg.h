#pragma once

#include <string>
#include <vector>

// Windows' own dialogs that the game opens: the file dialog for saving and
// loading, and the message boxes it can raise (replace this file?). They run
// their own message loop on the game thread, so the dispatch hook does not
// pump and game-thread commands cannot run; the pipe thread reads and answers
// them with window messages only, never game memory or game code.

// The dialog on top, as a JSON object, or empty when none is open.
// A window with no buttons and no filename box is not ready: this returns empty.
std::string SysDialogJson();
// True when a Windows dialog is on screen but has no buttons and no filename box yet.
bool SysDialogUnready();
// Types file into the name box when given, then presses the chosen button:
// by option index, by exact text, or OK when only file is given.
std::string SysDialogRespond(int option, const std::string& text, const std::string* file);
