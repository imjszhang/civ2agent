#include "game.h"
#include "server.h"

#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
    GameInit();
    StartServer();
  }
  return TRUE;
}
