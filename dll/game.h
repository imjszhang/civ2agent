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
std::string GameBeginEndTurnJson();
bool GameEndTurnPending();
bool GameEndTurnDone();
std::string GameFinishEndTurnJson();
void GameCancelEndTurn();

// Called on the game thread when a dialog appears under the active command.
bool GameTakeModalAbort(std::string& response_json);
void GameObserveDialog();
