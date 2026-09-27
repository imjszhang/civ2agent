#pragma once

#include <string>

constexpr int kProtocolVersion = 1;

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

std::string GameHelloJson();
std::string GameMenuJson();
std::string GameSnapshotJson(bool debug);
std::string GameEventsJson();
std::string GameActJson(const std::string& name, int unit, int city, int x, int y,
                        int id, int tax, int science, int button, const std::string& kind,
                        const std::string& order);
// end_turn and the unit acts post the game's own menu command or key and
// return an empty string. The command then stays pending until
// GamePollPending sees the result at the game's outermost message loop.
std::string GameBeginEndTurnJson();
std::string GameBeginSkipIntroJson();
bool GamePendingActive();
std::string GamePollPending(int depth);
void GameCancelPending();
// Safe from the pipe thread. Wakes Civ2UIA's MsgWaitForMultipleObjectsEx
// (QS_MOUSEBUTTON) so the game thread can run our hook again. press_enter
// is for the end-of-turn prompt; press_escape skips the opening movie.
void GameWakeUi(bool press_enter, bool press_escape = false);

// Called on the game thread when a dialog appears under the active command.
bool GameTakeModalAbort(std::string& response_json);
void GameObserveDialog();
