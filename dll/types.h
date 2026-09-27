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
constexpr std::uint32_t kVaPopup = 0x006CEC84;
constexpr std::uint32_t kVaPfdx = 0x00628350;
constexpr std::uint32_t kVaPfdy = 0x00628360;
constexpr std::uint32_t kVaMoveUnit = 0x00402829;
constexpr std::uint32_t kVaProcessUnit = 0x00402716;
constexpr std::uint32_t kVaAfterActive = 0x004016EF;
constexpr std::uint32_t kVaUnitCanMove = 0x0040273E;
constexpr std::uint32_t kVaProcessGoto = 0x00401145;
constexpr std::uint32_t kVaMapGetSquare = 0x00401BB3;
constexpr std::uint32_t kVaMapVisible = 0x00403C24;
constexpr std::uint32_t kVaCalcCity = 0x00402603;
constexpr std::uint32_t kVaCivHasTech = 0x00402E7D;
constexpr std::uint32_t kVaGetString = 0x00403387;
constexpr std::uint32_t kVaTurnToYear = 0x00403418;
constexpr std::uint32_t kVaBuildCity = 0x00489BE2;
constexpr std::uint32_t kVaClearBusy = 0x00484D3B;
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

constexpr int kDlgNumButtons = 0x34;
constexpr int kDlgPressed = 0xDC;
constexpr int kDlgTitle = 0x134;
constexpr int kDlgFirstText = 0x230;
constexpr int kDlgButtonControls = 0x274;
constexpr int kDlgButtonTexts = 0x294;
constexpr int kButtonStride = 0x3C;
constexpr int kButtonCode = 0x04;
constexpr int kButtonHwnd = 0x1C;
