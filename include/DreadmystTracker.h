#pragma once

#include <Windows.h>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// Shared memory name for IPC between DLL and GUI
#define TRACKER_SHARED_MEMORY_NAME "DreadmystTrackerSharedMemory"
#define TRACKER_MUTEX_NAME "DreadmystTrackerMutex"

// Structure shared between DLL and external GUI
struct SharedTrackerData {
  // Magic number to verify valid data (0 until DLL initializes it)
  uint32_t magic{0};

  // Player stats
  int totalKills{0};
  int totalLootItems{0};
  int64_t totalGold{0};
  int totalExp{0};
  int64_t goldSpent{0};   // Repair costs, purchases, etc.
  int64_t totalDamage{0}; // Total damage dealt (for DPS)

  // Party stats
  int partyKills{0};
  int partyLootItems{0};
  int64_t partyGold{0};
  int partyExp{0};

  // Loot by quality (indices 0-5 for QualityLv0-QualityLv5)
  int lootByQuality[6]{0, 0, 0, 0, 0, 0};

  // Last 10 loot entries (circular buffer)
  struct RecentLoot {
    char itemName[64]{};
    uint8_t quality{0};
    int amount{0};
    int64_t timestamp{0};
  } recentLoot[10]{};
  int recentLootIndex{0};

  // Last 10 kill entries (circular buffer)
  struct RecentKill {
    char mobName[64]{};
    int expGained{0};
    int64_t timestamp{0};
  } recentKills[10]{};
  int recentKillIndex{0};

  // Overlay visible flag (can be toggled from GUI)
  bool overlayVisible{false};

  // Session start time
  int64_t sessionStartTime{0};

  // Debug text for displaying probed buffer values
  char debugText[512]{};
};

namespace DreadmystTracker {

// Forward declarations from the game (we'll hook into these directly)
class World;
class ClientPlayer;
class ClientUnit;
class ClientObject;
class GameChat;
class LootWindow;
class Inventory;
class ItemIcon;
class GameIcon;
class Tooltip;

// Item quality enum matching game's ItemDefines::Quality
enum class ItemQuality : uint8_t {
  QualityLv0 = 0,
  QualityLv1 = 1,
  QualityLv2 = 2,
  QualityLv3 = 3,
  QualityLv4 = 4,
  QualityLv5 = 5
};

// Mirrors game's ItemDefines::ItemDefinition
struct ItemDefinition {
  uint16_t m_itemId{0};
  uint16_t m_affixId{0};
  uint8_t m_affixScore{0};
  uint8_t m_enchantLvl{0};
  uint8_t m_durability{0};
  uint16_t m_gem1{0};
  uint16_t m_gem2{0};
  uint16_t m_gem3{0};
  uint16_t m_gem4{0};
};

// Stats we track
struct CombatStats {
  int totalKills{0};
  int totalLootItems{0};
  int64_t totalGold{0};
  int totalExp{0};
  int64_t goldSpent{0};   // Repair costs, purchases
  int64_t totalDamage{0}; // Total damage dealt
  std::map<ItemQuality, int> lootByQuality;

  void reset() {
    totalKills = 0;
    totalLootItems = 0;
    totalGold = 0;
    totalExp = 0;
    goldSpent = 0;
    totalDamage = 0;
    lootByQuality.clear();
  }
};

struct LootEntry {
  ItemDefinition item;
  std::string itemName;
  ItemQuality quality;
  int amount{1};
  std::string looterName;
  int64_t timestamp{0};
};

struct KillEntry {
  std::string mobName;
  int64_t timestamp{0};
  int expGained{0};
  bool isPartyKill{false};
};

//=============================================================================
// GameBridge - Direct access to game objects
// Since we're injected, we can just call game functions directly!
//=============================================================================
class GameBridge {
public:
  static GameBridge &getInstance();

  // Find game objects (called once after injection)
  bool initialize();

  // Direct access to game state
  World *getWorld();
  ClientPlayer *getLocalPlayer();
  ClientUnit *getSelectedTarget();

  // Get player/party info directly from World
  int getLocalPlayerGuid();
  std::string getLocalPlayerName();
  std::vector<int> getPartyMemberGuids();
  bool isInParty();

  // Get stats directly from ClientPlayer
  int getPlayerHealth();
  int getPlayerMaxHealth();
  int getPlayerMana();
  int getPlayerMaxMana();
  int getPlayerLevel();
  int getPlayerExp();
  int getPlayerGold();

  // Get target info from World::selectedUnit
  std::string getTargetName();
  int getTargetHealth();
  int getTargetMaxHealth();
  bool isTargetHostile();

  // Access game's item database directly via ContentMgr
  std::string getItemName(uint16_t itemId);
  std::string getItemIcon(uint16_t itemId);
  ItemQuality getItemQuality(uint16_t itemId);
  std::string getNpcName(int entry);

  // Access the tooltip system - we can reuse the game's tooltip rendering!
  void showGameTooltip(const ItemDefinition &item, int x, int y);

  // Get hovered item from any GameIconList
  ItemDefinition *getHoveredItem();

private:
  GameBridge() = default;

  // Cached pointers to game singletons
  void *m_application{nullptr}; // sApplication
  void *m_contentMgr{nullptr};  // sContentMgr
  void *m_connector{nullptr};   // sConnector
};

//=============================================================================
// EventHooks - Hook into game events instead of parsing packets
//=============================================================================
class EventHooks {
public:
  static EventHooks &getInstance();

  bool install();
  void uninstall();

  // Callbacks
  std::function<void(const std::string &mobName, int exp)> onMobKilled;
  std::function<void(const LootEntry &)> onLootReceived;
  std::function<void(int amount)> onExpGained;
  std::function<void(int amount)> onGoldChanged;

private:
  EventHooks() = default;

  // We hook these game functions:
  // - Game::processPacket_Server_ExpNotify - exp gains
  // - Game::processPacket_Server_NotifyItemAdd - loot
  // - Game::processPacket_Server_DestroyObject - mob deaths
  // - ClientUnit::died - cleaner death detection

  bool hookExpNotify();
  bool hookNotifyItemAdd();
  bool hookUnitDeath();

  // Original function pointers
  static void *s_origExpNotify;
  static void *s_origNotifyItemAdd;
  static void *s_origUnitDied;
  static void *s_origPkNotify;
  static void *s_origSpentGold;
  static void *s_origCombatMsg;

  // Our hook functions
  static void __fastcall HookExpNotify(void *game, void *edx, void *data);
  static void __fastcall HookNotifyItemAdd(void *game, void *edx, void *data);
  static void __fastcall HookUnitDied(void *unit, void *edx);

  bool m_installed{false};
};

//=============================================================================
// OverlayRenderer - Can use game's own rendering system!
//=============================================================================
class OverlayRenderer {
public:
  static OverlayRenderer &getInstance();

  bool initialize();
  void shutdown();

  // Hook into game's render loop
  void render();

  // Panel visibility
  void showStatsPanel(bool show);
  void showLootPanel(bool show);
  void showKillPanel(bool show);

  void setPosition(int x, int y);
  void setOpacity(float opacity);

  // Update data to display
  void updateStats(const CombatStats &player, const CombatStats &party);
  void addLootEntry(const LootEntry &entry);
  void addKillEntry(const KillEntry &entry);

private:
  OverlayRenderer() = default;

  // We can use the game's own UI classes!
  // TextBox, Tooltip, Sprite are all available
  void renderStatsPanel();
  void renderLootHistory();
  void renderKillHistory();

  // Or just draw directly to the game's render target
  void drawText(const std::string &text, int x, int y, uint32_t color);
  void drawRect(int x, int y, int w, int h, uint32_t color);

  bool m_showStats{true};
  bool m_showLoot{true};
  bool m_showKills{true};

  int m_posX{10};
  int m_posY{10};
  float m_opacity{0.85f};

  CombatStats m_playerStats;
  CombatStats m_partyStats;
  std::vector<LootEntry> m_lootHistory;
  std::vector<KillEntry> m_killHistory;

  // Hook into World::render or Game::render
  static void *s_origWorldRender;
  static void __fastcall HookWorldRender(void *world, void *edx);
};

//=============================================================================
// Main Tracker - Much simpler now!
//=============================================================================
class Tracker {
public:
  static Tracker &getInstance();

  bool initialize();
  void shutdown();

  // Stats access
  const CombatStats &getPlayerStats() const { return m_playerStats; }
  const CombatStats &getPartyStats() const { return m_partyStats; }

  // Reset
  void resetStats();

  // Overlay control
  void toggleOverlay();
  bool isOverlayVisible() const { return m_overlayVisible; }

  // Hook callbacks
  void notifyExpGained(int amount);
  void notifyMobKilled(const std::string &name, int exp);
  void notifyLootReceived(const LootEntry &loot);
  void notifyGoldChanged(int amount);
  void notifyGoldSpent(int amount);
  void notifyDamageDealt(int amount);

private:
  Tracker() = default;

  // Event handlers (called by hooks)
  void onMobKilled(const std::string &name, int exp);
  void onLootReceived(const LootEntry &loot);
  void onExpGained(int amount);

  CombatStats m_playerStats;
  CombatStats m_partyStats;

  std::vector<LootEntry> m_lootHistory;
  std::vector<KillEntry> m_killHistory;

  bool m_initialized{false};
  bool m_overlayVisible{true};

  // Shared memory for external GUI
  HANDLE m_sharedMemHandle{nullptr};
  HANDLE m_mutexHandle{nullptr};
  SharedTrackerData *m_sharedData{nullptr};

  // Test data thread (for GUI verification)
  std::thread m_testThread;

  void updateSharedMemory();
  bool initSharedMemory();
  void cleanupSharedMemory();
};

} // namespace DreadmystTracker
