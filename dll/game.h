#pragma once

#include <string>

constexpr int kProtocolVersion = 2;

// One act request. Strings are UTF-8 from the pipe; -1 means not given.
struct ActArgs {
  std::string name;
  int unit = -1;
  int city = -1;
  int x = 0;
  int y = 0;
  int option = -1;  // respond: index into dialog.options
  std::string text;  // respond: option text
  std::string edit;  // respond: text to put in the edit box first
  bool has_edit = false;
  std::string order;
  std::string item;  // produce: option text
  int item_id = -1;  // produce: option id
  int tax = -1;
  int science = -1;
};

struct Gate {
  bool hash_ok = false;
  bool limits_ok = false;
  bool uia_loaded = false;
  bool in_game = false;
  int multi_type = -1;
  std::string exe_hash;
  std::string block_error;
};

void GameInit();
void GameLog(const char* fmt, ...);
Gate GameGate();
bool GameInMatch();
bool GameDialogOpen();
// The city window is showing; Modal: the game waits for it to close.
bool GameCityWindowOpen();
bool GameCityWindowModal();
// The tax rate window is open; the game always waits for it.
bool GameTaxWindowOpen();
// A window (not a dialog) holds the game until the player closes it.
bool GameWindowModal();

std::string GameHelloJson();
std::string GameMenuJson();
std::string GameSnapshotJson();
std::string GameEventsJson();
std::string GameActJson(const ActArgs& args);
// end_turn and the unit acts post the game's own menu command or key and
// return an empty string. The command then stays pending until
// GamePollPending sees the result at the game's outermost message loop.
std::string GameBeginEndTurnJson();
std::string GameBeginSkipIntroJson();
bool GamePendingActive();
std::string GamePollPending(int depth);
void GameCancelPending();
// Safe from the pipe thread, which also posts WM_NULL to wake the game
// thread. press_escape clicks and presses Escape to skip the opening movie;
// the end-of-turn prompt is answered from the game thread (GamePollPending).
void GameWakeUi(bool press_escape);

// Called on the game thread when a dialog appears under the active command.
bool GameTakeModalAbort(std::string& response_json);
void GameObserveDialog();
