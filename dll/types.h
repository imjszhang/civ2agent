#pragma once

// Layouts follow FoxAhead/Civ2-UI-Additions (MIT) src/Civ2Types.pas for
// Civilization II MGE 5.4.0f. Addresses below are virtual addresses at the
// preferred image base 0x400000. Reimplemented here; the Pascal sources are
// not copied.

#include <cstdint>
#include <cstddef>

constexpr std::uint32_t kImageBase = 0x00400000;
constexpr std::uint32_t kVaGame = 0x00655AE8;
constexpr std::uint32_t kVaUnits = 0x006560F0;
constexpr std::uint32_t kVaCities = 0x0064F340;
constexpr std::uint32_t kVaCivs = 0x0064C6A0;
constexpr std::uint32_t kVaMapHeader = 0x006D1160;
constexpr std::uint32_t kVaHumanCiv = 0x006D1DA0;
constexpr std::uint32_t kVaUnitSelected = 0x006D1DA8;
constexpr std::uint32_t kVaUnitTypes = 0x0064B1B8;
constexpr std::uint32_t kVaRules = 0x00627680;
constexpr std::uint32_t kVaPopup = 0x006CEC84;       // top of the popup stack
constexpr std::uint32_t kVaPopupStack = 0x00635A58;  // popups at [1..count] (control builder 0x5A211C)
constexpr std::uint32_t kVaPopupCount = 0x00635A9C;
constexpr std::uint32_t kVaPfdx = 0x00628350;
constexpr std::uint32_t kVaPfdy = 0x00628360;
constexpr std::uint32_t kVaUnitCanMove = 0x0040273E;
constexpr std::uint32_t kVaMapGetSquare = 0x00401BB3;
constexpr std::uint32_t kVaGetString = 0x00403387;
constexpr std::uint32_t kVaTurnToYear = 0x00403418;
constexpr std::uint32_t kVaMapGetCivData = 0x00403823;   // cdecl (x, y, civ) -> known features byte
constexpr std::uint32_t kVaGetAdvanceCost = 0x004030E9;  // cdecl (civ) -> beakers for the next advance
// cdecl (unit) -> movement left this turn, the game's own number (UnitCanMove
// uses it). Unit::move_points is movement already spent.
constexpr std::uint32_t kVaUnitMovesLeft = 0x005B2C3D;
constexpr std::uint32_t kVaCosmic = 0x0064BCC8;  // byte 0: road movement multiplier

constexpr std::uint16_t kUnitVeteran = 0x2000;
constexpr std::uint16_t kUnitAutomated = 0x8000;
constexpr std::uint16_t kFeatureDamage = 0x10;  // Game::custom_features: HP bars and damaged movement
constexpr std::uint8_t kFeatureUnit = 0x01;
constexpr std::uint8_t kFeatureCity = 0x02;
constexpr std::uint8_t kFeatureAirbase = 0x42;
constexpr std::int32_t kTreatyContact = 0x01;
constexpr std::int32_t kTreatyCeaseFire = 0x02;
constexpr std::int32_t kTreatyPeace = 0x04;
constexpr std::int32_t kTreatyAlliance = 0x08;
constexpr std::int32_t kTreatyEmbassy = 0x80;
constexpr std::int32_t kTreatyWar = 0x2000;
constexpr int kUnitSlots = 2048;
constexpr int kCitySlots = 256;
constexpr int kCivSlots = 8;
constexpr int kCivStride = 0x594;

#pragma pack(push, 1)

struct Game {
  std::uint16_t custom_features;
  std::int32_t graphic_options;
  std::uint16_t word_flags;
  std::uint16_t map_flags;
  std::uint16_t word_af2;
  std::uint16_t tutorials;
  std::uint16_t word_af6;
  std::int16_t turn;
  std::uint16_t year;
  std::uint16_t word_afc;
  std::int16_t active_unit;
  std::uint16_t word_b00;
  std::uint8_t multi_type;
  std::uint8_t human_civ_byte;
  std::uint8_t byte_b04;
  std::int8_t some_civ;
  std::uint8_t byte_b06;
  std::uint8_t reveal_map;
  std::uint8_t difficulty;
  std::uint8_t barbarian;
  std::uint8_t active_players;
  std::uint8_t human_players;
  std::uint8_t active_players_start;
  std::uint8_t enemies;
  std::uint8_t byte_b0e;
  std::uint8_t byte_b0f;
  std::uint16_t word_b10;
  std::uint16_t word_b12;
  std::uint16_t peace_turns;
  std::int16_t total_units;
  std::uint16_t total_cities;
  std::uint16_t word_b1a;
  std::uint16_t word_b1c;
  std::uint8_t techs_first[100];
  std::uint8_t techs_discovered[100];
  std::uint8_t rest[0x4C];
};

struct Unit {
  std::uint16_t x;
  std::uint16_t y;
  std::uint16_t attributes;
  std::uint8_t type;
  std::int8_t civ;
  std::int8_t move_points;
  std::uint8_t visibility;
  std::uint8_t hp_lost;
  std::uint8_t move_direction;
  char debug_symbol;
  std::int8_t counter;
  std::uint8_t move_iteration;
  std::int8_t orders;
  std::uint8_t home_city;
  std::uint8_t pad11;
  std::uint16_t goto_x;
  std::uint16_t goto_y;
  std::uint16_t prev_stack;
  std::uint16_t next_stack;
  std::int32_t id;
  std::uint16_t pad1e;
};

struct City {
  std::int16_t x;
  std::int16_t y;
  std::uint32_t attributes;
  std::uint8_t owner;
  std::int8_t size;
  std::uint8_t founder;
  std::uint8_t turns_captured;
  std::uint8_t known_to;
  std::int8_t revealed_size[8];
  std::uint8_t unknown_15;
  std::uint32_t specialists;
  std::int16_t food_storage;
  std::int16_t build_progress;
  std::int16_t base_trade;
  char name[16];
  std::int32_t workers;
  std::uint8_t improvements[5];
  std::int8_t building;
  std::int8_t trade_routes;
  std::int8_t supplied[3];
  std::int8_t demanded[3];
  std::int8_t commodity[3];
  std::int16_t trade_partner[3];
  std::int16_t science;
  std::int16_t tax;
  std::int16_t trade;
  std::uint8_t total_food;
  std::uint8_t total_shield;
  std::uint8_t happy;
  std::uint8_t unhappy;
  std::int32_t id;
};

struct CivHead {
  std::uint16_t flags;
  std::int32_t gold;
  std::uint16_t leader;
  std::uint16_t beakers;
  std::int16_t researching;
  std::int16_t capital_x;
  std::int16_t turn_city;
  std::uint8_t techs_count;
  std::uint8_t future_techs;
  std::int8_t unk12;
  std::uint8_t science_rate;
  std::uint8_t tax_rate;
  std::uint8_t government;
  std::int8_t senate;
  std::uint8_t unk17[4];
  std::uint8_t unk1b;
  std::uint16_t unk1c;
  std::uint8_t reputation;
  std::uint8_t unk1f;
  std::int32_t treaties[8];
  std::uint8_t attitude[8];
};

struct MapHeader {
  std::int16_t size_x;
  std::int16_t size_y;
  std::int16_t area;
  std::int16_t flat;
  std::int16_t seed;
  std::int16_t array_w;
  std::int16_t array_h;
};

struct MapSquare {
  std::uint8_t terrain;
  std::uint8_t features;
  std::uint8_t city_radii;
  std::uint8_t mass;
  std::uint8_t visibility;
  std::uint8_t ownership;
};

struct UnitType {
  std::uint32_t string_index;
  std::uint32_t abilities;
  std::uint8_t until_;
  std::uint8_t domain;
  std::uint8_t move;
  std::uint8_t range;
  std::uint8_t att;
  std::uint8_t def;
  std::uint8_t hp;
  std::uint8_t fire;
  std::uint8_t cost;
  std::uint8_t hold;
  std::uint8_t role;
  std::uint8_t preq;
};

struct RulesCivilize {
  char name[4];
  std::int32_t text_index;
  std::int8_t unk08;
  std::int8_t preq_not_no;
  std::int8_t ai_value;
  std::int8_t modifier;
  std::int8_t category;
  std::int8_t epoch;
  std::int8_t preq[2];
};

#pragma pack(pop)

static_assert(sizeof(Game) == 0x14A, "TGame");
static_assert(offsetof(Game, turn) == 0x10, "turn");
static_assert(offsetof(Game, year) == 0x12, "year");
static_assert(offsetof(Game, active_unit) == 0x16, "active unit");
static_assert(offsetof(Game, multi_type) == 0x1A, "multi");
static_assert(offsetof(Game, total_units) == 0x2E, "units");
static_assert(offsetof(Game, total_cities) == 0x30, "cities");
static_assert(offsetof(Game, techs_discovered) == 0x9A, "techs");

static_assert(sizeof(Unit) == 0x20, "TUnit");
static_assert(offsetof(Unit, orders) == 0x0F, "orders");
static_assert(offsetof(Unit, id) == 0x1A, "unit id");

static_assert(sizeof(City) == 0x58, "TCity");
static_assert(offsetof(City, name) == 0x20, "city name");
static_assert(offsetof(City, building) == 0x39, "building");
static_assert(offsetof(City, id) == 0x54, "city id");

static_assert(sizeof(CivHead) == 0x48, "civ head");
static_assert(offsetof(CivHead, gold) == 0x02, "gold");
static_assert(offsetof(CivHead, researching) == 0x0A, "research");
static_assert(offsetof(CivHead, science_rate) == 0x13, "science");
static_assert(offsetof(CivHead, tax_rate) == 0x14, "tax");
static_assert(offsetof(CivHead, treaties) == 0x20, "treaties");

static_assert(sizeof(MapSquare) == 6, "square");
static_assert(sizeof(UnitType) == 0x14, "unit type");
static_assert(offsetof(UnitType, role) == 0x12, "role");
static_assert(sizeof(RulesCivilize) == 0x10, "rules");

// Dialog object (*kVaPopup). Offsets and flags are from the control builder
// 0x5A211C and the run body 0x5A5F34; names follow Civ2UIA's TDialogWindow.
constexpr int kDlgNumEdits = 0x24;
constexpr int kDlgNumLines = 0x28;
constexpr int kDlgNumListItems = 0x2C;
constexpr int kDlgNumButtonsStd = 0x30;
constexpr int kDlgNumButtons = 0x34;
constexpr int kDlgFlags = 0x3C;
constexpr int kDlgListPage = 0x48;
constexpr int kDlgTitle = 0x134;
constexpr int kDlgFirstText = 0x230;
constexpr int kDlgListSelected = 0x220;
constexpr int kDlgListHead = 0x228;
constexpr int kDlgEditControls = 0x270;
constexpr int kDlgSysListControls = 0x278;
constexpr int kDlgButtonTexts = 0x294;
constexpr int kDlgButtonLayout = 0x2AC;  // {std type, left x} per button
constexpr int kListFlags = 0x00;
constexpr int kListId = 0x04;
constexpr int kListText = 0x08;
constexpr int kListPage = 0x0C;
constexpr int kListNext = 0x10;
constexpr int kEditControlStride = 0x48;
constexpr int kControlHwnd = 0x1C;
constexpr int kEditFilter = 0x34;  // optional cdecl (control +4, char) -> accept

constexpr std::uint32_t kDlgCheckboxes = 0x4;
constexpr std::uint32_t kDlgNoWait = 0x8;  // runs no loop of its own: advice that stays up without holding the game
constexpr std::uint32_t kDlgCreated = 0x20;
constexpr std::uint32_t kDlgCreatedParts = 0x200;
constexpr std::uint32_t kDlgClosing = 0x400;
constexpr std::uint32_t kDlgListbox = 0x1000;
constexpr std::uint32_t kDlgChoose = 0x2000;
constexpr std::uint32_t kDlgSystemListbox = 0x40000;
constexpr std::uint32_t kDlgTwoLists = 0x400000;
constexpr std::uint32_t kListDisabled = 0x1;
constexpr int kDlgMaxRows = 40;
constexpr int kButtonIdBase = 0x64;

// The game's own handlers for one dialog answer; each acts on *kVaPopup.
constexpr std::uint32_t kVaDlgButton = 0x005A3E56;      // cdecl (control id)
constexpr std::uint32_t kVaDlgChooseRow = 0x005A3BAE;   // cdecl (unused, row index)
constexpr std::uint32_t kVaDlgSelectRow = 0x005A17E9;   // thiscall (dlg; record, redraw)
constexpr std::uint32_t kVaDlgClose = 0x005A3C58;       // cdecl ()
constexpr std::uint32_t kVaDlgSysListDone = 0x005A3C83; // cdecl ()
constexpr std::uint32_t kVaPopupCaptions = 0x00635A48;  // OK, Help, Cancel
constexpr int kStdButtonOk = 0;      // standard button types at kDlgButtonLayout
constexpr int kStdButtonCancel = 2;

// City window (TCityWindow at 0x6A91B8). Static analysis of 0x509590,
// 0x50A473 and 0x50BC4F. Open means Hidden == 0 and CityIndex != -1.
constexpr std::uint32_t kVaCityWindow = 0x006A91B8;
constexpr int kCityWinCityIndex = 0x159C;
constexpr int kCityWinHidden = 0x15A4;
constexpr int kCityWinModalMode = 0x15AC;
// Non-zero while the game shows the city window modally (after founding or
// taking a city, from a turn report) and waits for it to close.
constexpr std::uint32_t kVaLockCityWindow = 0x0062EDF8;
constexpr std::uint32_t kVaCityWindowShow = 0x00402E78;  // thiscall (window; city), returns at once when unlocked
constexpr std::uint32_t kVaCityChange = 0x004021D5;      // cdecl (unused): the Change button, runs @PRODUCTION
constexpr std::uint32_t kVaCityWindowExit = 0x004034E5;  // cdecl (): the Exit button
constexpr std::uint32_t kVaImprovements = 0x0064C488;    // 67 x 8 bytes, string index at +0

// A left click on one of the player's units (map click handler 0x410F77):
// leave moving mode, put the cursor on the tile, activate the top unit there
// (a stack opens the units-list popup), centre the map on the tile.
constexpr std::uint32_t kVaLeaveMovingMode = 0x004897FA;  // cdecl (0)
constexpr std::uint32_t kVaSetCursor = 0x00410D98;        // cdecl (x, y)
constexpr std::uint32_t kVaActivateAtCursor = 0x0058D442; // cdecl (): clears orders, AfterActiveUnitChanged
constexpr std::uint32_t kVaCenterView = 0x00403404;       // thiscall (map window; x, y)
constexpr std::uint32_t kVaMapWindow = 0x0066C7A8;

// Tax rate window (TTaxWindow, on ShowTaxRate's stack while open). Static
// analysis of 0x40CD64, 0x40BDAC and 0x5CD149.
constexpr std::uint32_t kVaTaxWindowOpen = 0x0063CBB0;  // int, non-zero while open
constexpr std::uint32_t kVaTaxWindowPtr = 0x0063CBB4;   // TTaxWindow*, stale after close
constexpr int kTaxWinCiv = 0x2D8;
constexpr int kTaxWinMaxRate = 0x2DC;
constexpr int kTaxWinTax = 0x2E0;      // live values, tenths
constexpr int kTaxWinLux = 0x2E4;
constexpr int kTaxWinScience = 0x2E8;
constexpr int kTaxWinTaxBar = 0x374;   // scroll bar HWNDs (MSScrollBarClass, range 0..10)
constexpr int kTaxWinLuxBar = 0x3B4;
constexpr int kTaxWinScienceBar = 0x3F4;
constexpr std::uint32_t kVaTaxWindowOk = 0x0040C3A5;  // cdecl (): the OK button; Enter and Esc do the same
constexpr std::uint32_t kVaMenuExec = 0x0040385A;     // cdecl (menu, command), what a menu click runs
constexpr int kMenuKingdom = 2;
constexpr int kMenuTaxRate = 0x201;
// @PRODUCTION record ids: unit type, 0x3E + improvement, 0x65 + wonder (= 0x3E + 39 + wonder).
constexpr int kProductionImprovementBase = 0x3E;

// Names the game shows for a civ (the status bar, the save and load
// messages "%2 的 %0 %1"). cdecl (civ) -> game string, no side effects.
constexpr std::uint32_t kVaCivLeaderName = 0x00493B10;   // 尤利乌斯·恺撒, or the name typed at the start
constexpr std::uint32_t kVaCivLeaderTitle = 0x00493BA6;  // by government and gender: 皇帝
constexpr std::uint32_t kVaCivName = 0x00493C7D;         // as in the tribe list: 罗马人

// Saving and loading (static analysis of 0x47758C and 0x477D8C). Both ask
// for the file with GetSaveFileNameA / GetOpenFileNameA (0x4731D2: flags
// 0x80E, open adds FILEMUSTEXIST; no hook; a 256-byte name buffer). That
// dialog runs Windows' own loop, which does not dispatch through civ2.exe's
// import, so the pump below does not run while it is open (sysdlg.h).
// The loader 0x475666 keeps the folder of the loaded save (0x64BB08), and the
// end-of-turn autosave 0x488937 writes <2 bytes of leader name>_自动.SAV
// there; a new game sets it to the game folder (0x51D9A0).
