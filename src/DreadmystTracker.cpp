#define _CRT_SECURE_NO_WARNINGS
#include "DreadmystTracker.h"
#include <MinHook.h>
#include <chrono>
#include <psapi.h>
#include <regex>
#include <string>
#include <vector>
#include <windows.h>

namespace DreadmystTracker {

//=============================================================================
// Chat Message Parser - Parse loot/kill/exp from chat messages
//=============================================================================
class ChatParser {
public:
  static ChatParser &getInstance() {
    static ChatParser instance;
    return instance;
  }

  // Parse a chat message and update tracker stats
  void parseMessage(const std::string &message) {
    if (!g_trackerInstance)
      return;

    // Parse "You receive: [Item Name]" or "You receive: [Item Name] xN"
    parseLootMessage(message);

    // Parse "You gained X experience" or similar
    parseExpMessage(message);

    // Parse kill messages (varies by game)
    parseKillMessage(message);

    // Parse gold messages
    parseGoldMessage(message);
  }

  void setTracker(Tracker *tracker) { g_trackerInstance = tracker; }

private:
  ChatParser() = default;
  Tracker *g_trackerInstance = nullptr;

  void parseLootMessage(const std::string &msg) {
    // Pattern: "You receive: [Item] xN" or "You receive: [Item]"
    // Also handles: "You receive: [Gold] x5"
    static std::regex lootRegex(R"(You receive:\s*\[([^\]]+)\](?:\s*x(\d+))?)",
                                std::regex::icase);

    std::smatch match;
    if (std::regex_search(msg, match, lootRegex)) {
      std::string itemName = match[1].str();
      int amount = 1;
      if (match[2].matched) {
        amount = std::stoi(match[2].str());
      }

      // Check if it's gold
      if (itemName == "Gold" || itemName == "gold") {
        g_trackerInstance->notifyGoldChanged(amount);
      } else {
        // Create loot entry
        LootEntry entry;
        entry.itemName = itemName;
        entry.amount = amount;
        entry.quality = guessQuality(itemName);
        entry.timestamp =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        g_trackerInstance->notifyLootReceived(entry);
      }
    }
  }

  void parseExpMessage(const std::string &msg) {
    // Pattern: "You gained X experience" or "You gain X exp" or "+X XP"
    static std::regex expRegex(
        R"((?:You (?:gained?|received?)|got|\+)\s*(\d+)\s*(?:experience|exp|xp))",
        std::regex::icase);

    std::smatch match;
    if (std::regex_search(msg, match, expRegex)) {
      int exp = std::stoi(match[1].str());
      g_trackerInstance->notifyExpGained(exp);
    }
  }

  void parseKillMessage(const std::string &msg) {
    // Pattern: "You killed [Monster Name]" or "You have slain [Name]"
    // Also: "[Monster] has been defeated"
    static std::regex killRegex(
        R"((?:You (?:killed?|slain|defeated)|has been defeated)\s*(?:\[([^\]]+)\]|(\w+)))",
        std::regex::icase);

    std::smatch match;
    if (std::regex_search(msg, match, killRegex)) {
      std::string mobName = match[1].matched ? match[1].str() : match[2].str();
      g_trackerInstance->notifyMobKilled(mobName, 0);
    }
  }

  void parseGoldMessage(const std::string &msg) {
    // Pattern: "You received X Gold" (separate from loot format)
    static std::regex goldRegex(
        R"((?:You (?:received?|got|looted))\s*(\d+)\s*(?:Gold|gold|coins?))",
        std::regex::icase);

    std::smatch match;
    if (std::regex_search(msg, match, goldRegex)) {
      int gold = std::stoi(match[1].str());
      g_trackerInstance->notifyGoldChanged(gold);
    }
  }

  // Guess item quality based on name (color codes in name, or keywords)
  ItemQuality guessQuality(const std::string &itemName) {
    // Check for common quality indicators in item names
    if (itemName.find("Legendary") != std::string::npos ||
        itemName.find("Divine") != std::string::npos)
      return ItemQuality::QualityLv5;
    if (itemName.find("Epic") != std::string::npos ||
        itemName.find("Imperial") != std::string::npos)
      return ItemQuality::QualityLv4;
    if (itemName.find("Rare") != std::string::npos ||
        itemName.find("Holy") != std::string::npos)
      return ItemQuality::QualityLv3;
    if (itemName.find("Uncommon") != std::string::npos ||
        itemName.find("Large") != std::string::npos ||
        itemName.find("Curious") != std::string::npos)
      return ItemQuality::QualityLv2;
    return ItemQuality::QualityLv1; // Common
  }
};

//=============================================================================
// Chat Hook - Hook the game's chat/message display function
//=============================================================================
typedef void(__thiscall *OrigAddMessage_t)(void *thisPtr, const char *message,
                                           int color);
static OrigAddMessage_t g_origAddMessage = nullptr;

// Forward declaration for chat filter access (defined later with other globals)
static SharedTrackerData *g_sharedData;

// Hook function for chat messages
void __fastcall HookedAddMessage(void *thisPtr, void *edx, const char *message,
                                 int color) {
  // Check if chat filter is enabled
  bool shouldFilter = false;
  if (message && g_sharedData && g_sharedData->chatFilterEnabled &&
      g_sharedData->chatFilterTerms[0] != '\0') {
    // Parse comma-separated filter terms and check if message contains any
    char filterCopy[512];
    strncpy_s(filterCopy, sizeof(filterCopy), g_sharedData->chatFilterTerms,
              sizeof(filterCopy) - 1);
    filterCopy[sizeof(filterCopy) - 1] = '\0';

    // Convert message to lowercase for case-insensitive matching
    std::string msgLower(message);
    for (auto &c : msgLower)
      c = (char)tolower(c);

    // Check each filter term
    char *context = nullptr;
    char *token = strtok_s(filterCopy, ",", &context);
    while (token != nullptr) {
      // Trim leading/trailing whitespace
      while (*token == ' ')
        token++;
      char *end = token + strlen(token) - 1;
      while (end > token && *end == ' ')
        *end-- = '\0';

      if (strlen(token) > 0) {
        // Convert term to lowercase
        std::string termLower(token);
        for (auto &c : termLower)
          c = (char)tolower(c);

        if (msgLower.find(termLower) != std::string::npos) {
          shouldFilter = true;
          break;
        }
      }
      token = strtok_s(nullptr, ",", &context);
    }
  }

  // If message should be filtered, skip calling original (don't display it)
  if (shouldFilter) {
    return;
  }

  // Call original to display message
  if (g_origAddMessage) {
    g_origAddMessage(thisPtr, message, color);
  }

  // Parse the message for tracking
  if (message) {
    ChatParser::getInstance().parseMessage(std::string(message));
  }
}

// Alternative: Hook printf-style message function
typedef void(__cdecl *OrigPrintMessage_t)(const char *format, ...);
static OrigPrintMessage_t g_origPrintMessage = nullptr;

//=============================================================================
// Pattern signatures for finding game functions/objects
// These are based on the game's source code structure
//=============================================================================

// sApplication is a global singleton - we find it by pattern
// In the game: #define sApplication (Application::getInstance())
constexpr const char *PATTERN_APPLICATION = "A1 ?? ?? ?? ?? 85 C0 74 ?? 8B 40";

// sContentMgr singleton
constexpr const char *PATTERN_CONTENTMGR = "A1 ?? ?? ?? ?? 8B ?? ?? 85 C0";

// Game::processPacket_Server_ExpNotify
constexpr const char *PATTERN_EXP_NOTIFY =
    "55 8B EC 83 EC ?? 56 8B F1 8D 4D ?? E8 ?? ?? ?? ?? 8B 45";

// Game::processPacket_Server_NotifyItemAdd
constexpr const char *PATTERN_ITEM_NOTIFY =
    "55 8B EC 81 EC ?? ?? ?? ?? 53 56 57 8D 85";

// World::render (we hook this to draw our overlay)
constexpr const char *PATTERN_WORLD_RENDER =
    "55 8B EC 83 EC ?? 53 56 8B F1 57 E8 ?? ?? ?? ?? 8B";

//=============================================================================
// Utility: Pattern scanning
//=============================================================================
void *ScanPattern(const char *pattern) {
  HMODULE mod = GetModuleHandle(nullptr);
  if (!mod)
    return nullptr;

  MODULEINFO info;
  if (!GetModuleInformation(GetCurrentProcess(), mod, &info, sizeof(info))) {
    return nullptr;
  }

  uint8_t *base = (uint8_t *)info.lpBaseOfDll;
  size_t size = info.SizeOfImage;

  // Parse pattern string into bytes
  std::vector<std::pair<uint8_t, bool>> bytes;
  const char *p = pattern;
  while (*p) {
    while (*p == ' ')
      p++;
    if (!*p)
      break;

    if (*p == '?') {
      bytes.push_back({0, true}); // wildcard
      while (*p == '?')
        p++;
    } else {
      char hex[3] = {p[0], p[1], 0};
      bytes.push_back({(uint8_t)strtol(hex, nullptr, 16), false});
      p += 2;
    }
  }

  // Scan
  for (size_t i = 0; i < size - bytes.size(); i++) {
    bool match = true;
    for (size_t j = 0; j < bytes.size(); j++) {
      if (!bytes[j].second && base[i + j] != bytes[j].first) {
        match = false;
        break;
      }
    }
    if (match)
      return base + i;
  }

  return nullptr;
}

//=============================================================================
// GameBridge Implementation
//=============================================================================
GameBridge &GameBridge::getInstance() {
  static GameBridge instance;
  return instance;
}

bool GameBridge::initialize() {
  // Find sApplication
  void *appPattern = ScanPattern(PATTERN_APPLICATION);
  if (appPattern) {
    // The pattern points to: mov eax, [address]
    // So we read the address from offset 1
    m_application = *(void **)((uint8_t *)appPattern + 1);
  }

  // Find sContentMgr similarly
  void *cmPattern = ScanPattern(PATTERN_CONTENTMGR);
  if (cmPattern) {
    m_contentMgr = *(void **)((uint8_t *)cmPattern + 1);
  }

  return m_application != nullptr;
}

World *GameBridge::getWorld() {
  if (!m_application)
    return nullptr;

  // Application has a Game member, Game has World as a child render object
  // Based on: auto world =
  // dynamic_pointer_cast<World>(getRenderObject(RoWorld));
  //
  // We need the actual offsets from the compiled game, but the structure is:
  // Application -> m_game (Game*) -> getRenderObject(RoWorld) -> World*
  //
  // For now, return nullptr - in real implementation we'd know the offsets
  return nullptr;
}

ClientPlayer *GameBridge::getLocalPlayer() {
  World *world = getWorld();
  if (!world)
    return nullptr;

  // World::myself() returns m_myself which is shared_ptr<ClientPlayer>
  // We'd call world->myself() directly since we're in the same process
  return nullptr;
}

// These would all call directly into game functions once we have proper offsets
int GameBridge::getPlayerHealth() { return 0; }
int GameBridge::getPlayerMaxHealth() { return 0; }
int GameBridge::getPlayerLevel() { return 0; }
int GameBridge::getPlayerExp() { return 0; }
int GameBridge::getPlayerGold() { return 0; }

std::string GameBridge::getItemName(uint16_t itemId) {
  // In the game this is: sContentMgr->db("item_template").data(itemId, "name")
  // We can call this directly!
  return "Unknown Item";
}

ItemQuality GameBridge::getItemQuality(uint16_t itemId) {
  // sContentMgr->db("item_template").data(itemId, "quality")
  return ItemQuality::QualityLv1;
}

bool GameBridge::isInParty() {
  // Would check World::m_party or similar
  return false;
}

//=============================================================================
// EventHooks Implementation - Using MinHook for detouring
//=============================================================================

// Function pointers for original functions
typedef void(__thiscall *OrigExpNotify_t)(void *thisPtr, void *data);
typedef void(__thiscall *OrigNotifyItemAdd_t)(void *thisPtr, void *data);
typedef void(__thiscall *OrigPkNotify_t)(void *thisPtr, void *data);
// GameChat::recvMsg(const string& msg, const string& from, Channels c,
// ItemDefinition* linkedItem)
typedef void(__thiscall *OrigRecvMsg_t)(void *thisPtr, void *msgStr,
                                        void *fromStr, int channel,
                                        void *linkedItem);

static OrigExpNotify_t g_origExpNotify = nullptr;
static OrigNotifyItemAdd_t g_origNotifyItemAdd = nullptr;
static OrigPkNotify_t g_origPkNotify = nullptr;
static OrigRecvMsg_t g_origRecvMsg = nullptr;

// Discovered function addresses from Dreadmyst.exe analysis:
// Game::processPacket_Server_ExpNotify at VA 0x0045E320
static constexpr DWORD EXP_NOTIFY_VA = 0x0045E320;
// Game::processPacket_Server_NotifyItemAdd at VA 0x004673C0
static constexpr DWORD ITEM_NOTIFY_VA = 0x004673C0;
// Game::processPacket_Server_PkNotify at VA 0x0045DE50
static constexpr DWORD PK_NOTIFY_VA = 0x0045DE50;
// Game::processPacket_Server_SpentGold at VA 0x0045EDD0
static constexpr DWORD GOLD_NOTIFY_VA = 0x0045EDD0;
// GameChat::recvMsg (FUN_00471e60) - for chat filtering
static constexpr DWORD RECVMSG_VA = 0x00471e60;
// Game::processPacket_Server_CombatMsg at VA 0x00468110 - for DPS tracking
static constexpr DWORD COMBAT_MSG_VA = 0x00468110;

// Static tracker reference for hooks (Must be declared before hooks)
static Tracker *g_trackerInstance = nullptr;

// Function pointer
typedef void(__thiscall *OrigSpentGold_t)(void *thisPtr, void *data);
static OrigSpentGold_t g_origSpentGold = nullptr;

// Hook function
void __fastcall HookedSpentGold(void *thisPtr, void *edx, void *data) {
  if (g_origSpentGold) {
    g_origSpentGold(thisPtr, data);
  }

  if (g_trackerInstance) {
    g_trackerInstance->notifyGoldChanged(0); // Placeholder to trigger update
  }
}

// Combat message hook for DPS tracking
typedef void(__thiscall *OrigCombatMsg_t)(void *thisPtr, void *data);
static OrigCombatMsg_t g_origCombatMsg = nullptr;

// StlBuffer is a class that wraps packet data. Common layout:
// struct StlBuffer {
//   char* m_data;      // offset 0: pointer to raw data
//   size_t m_readPos;  // offset 4: current read position
//   size_t m_size;     // offset 8: total size
// };
// GP_Server_CombatMsg packet fields (after unpacking):
// - m_targetGuid (int32)
// - m_casterGuid (int32)
// - m_amount (int32) - negative = damage
// - m_spellId (uint16)
// - m_spellEffect (uint8)
// - m_spellResult (uint8)
// - etc...

// Debug counter for combat events
static int g_combatMsgCount = 0;
// Debug buffer for packet data
static char g_combatDebug[512] = "No combat yet";
// Player GUID for filtering (0 = not yet captured)
static int32_t g_playerGuid = 0;

void __fastcall HookedCombatMsg(void *thisPtr, void *edx, void *data) {
  g_combatMsgCount++;

  if (data) {
    // Dump first 32 bytes of data as hex to understand structure
    uint8_t *bytes = (uint8_t *)data;
    _snprintf_s(g_combatDebug, sizeof(g_combatDebug), _TRUNCATE,
                "Cnt:%d %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X "
                "%02X%02X%02X%02X",
                g_combatMsgCount, bytes[0], bytes[1], bytes[2], bytes[3],
                bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9],
                bytes[10], bytes[11], bytes[12], bytes[13], bytes[14],
                bytes[15]);

    // Don't try to extract damage yet - we need to see the hex first
  }

  // Call original
  if (g_origCombatMsg) {
    g_origCombatMsg(thisPtr, data);
  }
}

// Global debug string for GUI display
static char g_debugText[512] = "Waiting for exp event...";
static int g_expEventCount = 0;

void __fastcall HookedExpNotify(void *thisPtr, void *edx, void *data) {
  // Call original first
  if (g_origExpNotify) {
    g_origExpNotify(thisPtr, data);
  }

  // Count mob kills (exp is tracked via addLine hook)
  g_expEventCount++;
  if (g_trackerInstance) {
    g_trackerInstance->notifyMobKilled("Enemy", 0);
  }
}

// Hook for GameChat::recvMsg - filters chat messages before display
void __fastcall HookedRecvMsg(void *thisPtr, void *edx, void *msgStr,
                              void *fromStr, int channel, void *linkedItem) {
  // Debug: track filter state
  static int filterCheckCount = 0;
  filterCheckCount++;

  // Always call original first to ensure game stability
  // We check shouldBlock first, then decide whether to call
  bool shouldBlock = false;

  __try {
    // Check if chat filter is enabled and has terms
    bool filterEnabled = (g_sharedData && g_sharedData->chatFilterEnabled &&
                          g_sharedData->chatFilterTerms[0] != '\0');

    if (filterEnabled && msgStr) {
      // Safely extract string - use SEH to catch any access violations
      char *strPtr = nullptr;

      // Check if SSO (small string optimization) or heap allocated
      // MSVC std::string layout: if capacity < 16, string is inline (SSO)
      uint32_t *ssoBuf = (uint32_t *)msgStr;
      uint32_t capacity = ssoBuf[5]; // capacity at offset 20

      if (capacity < 16) {
        strPtr = (char *)msgStr; // SSO inline buffer
      } else {
        strPtr = *(char **)msgStr; // Heap pointer at offset 0
      }

      // Validate pointer before use
      if (strPtr && (uintptr_t)strPtr > 0x10000 &&
          (uintptr_t)strPtr < 0x7FFFFFFF) {
        // Convert message to lowercase for case-insensitive matching
        char msgLower[512];
        strncpy_s(msgLower, sizeof(msgLower), strPtr, sizeof(msgLower) - 1);
        msgLower[sizeof(msgLower) - 1] = '\0';
        for (char *p = msgLower; *p; p++)
          *p = (char)tolower(*p);

        // Parse comma-separated filter terms
        char filterCopy[512];
        strncpy_s(filterCopy, sizeof(filterCopy), g_sharedData->chatFilterTerms,
                  sizeof(filterCopy) - 1);
        filterCopy[sizeof(filterCopy) - 1] = '\0';

        char *context = nullptr;
        char *token = strtok_s(filterCopy, ",", &context);
        while (token != nullptr) {
          // Trim whitespace
          while (*token == ' ')
            token++;
          char *end = token + strlen(token) - 1;
          while (end > token && *end == ' ')
            *end-- = '\0';

          if (strlen(token) > 0) {
            // Convert term to lowercase
            char termLower[64];
            strncpy_s(termLower, sizeof(termLower), token,
                      sizeof(termLower) - 1);
            termLower[sizeof(termLower) - 1] = '\0';
            for (char *p = termLower; *p; p++)
              *p = (char)tolower(*p);

            if (strstr(msgLower, termLower) != nullptr) {
              shouldBlock = true;
              break;
            }
          }
          token = strtok_s(nullptr, ",", &context);
        }
      }
    }

    // Write debug info
    sprintf_s(g_debugText, sizeof(g_debugText),
              "Filter: %s, Block: %s, Checks: %d",
              (g_sharedData && g_sharedData->chatFilterEnabled) ? "ON" : "OFF",
              shouldBlock ? "YES" : "NO", filterCheckCount);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // On any exception, don't block - let original handle it
    shouldBlock = false;
    sprintf_s(g_debugText, sizeof(g_debugText), "Filter: EXCEPTION caught");
  }

  // If message matches filter, block it
  if (shouldBlock) {
    return; // Don't call original - block message
  }

  // Call original to display message
  if (g_origRecvMsg) {
    g_origRecvMsg(thisPtr, msgStr, fromStr, channel, linkedItem);
  }
}

// Our hook function for ItemNotify
void __fastcall HookedItemNotify(void *thisPtr, void *edx, void *data) {
  if (g_origNotifyItemAdd) {
    g_origNotifyItemAdd(thisPtr, data);
  }

  if (g_trackerInstance) {
    // Notify loot received (Generic item for now)
    LootEntry entry;
    entry.item.m_itemId = 0;
    entry.itemName = "Looted Item";
    entry.quality = ItemQuality::QualityLv1; // Common
    entry.amount = 1;
    g_trackerInstance->notifyLootReceived(entry);
  }
}

// Our hook function for PkNotify
void __fastcall HookedPkNotify(void *thisPtr, void *edx, void *data) {
  if (g_origPkNotify) {
    g_origPkNotify(thisPtr, data);
  }

  if (g_trackerInstance) {
    // Notify mob killed
    g_trackerInstance->notifyMobKilled("Enemy", 0);
  }
}

void *EventHooks::s_origExpNotify = nullptr;
void *EventHooks::s_origNotifyItemAdd = nullptr;
void *EventHooks::s_origUnitDied = nullptr;
void *EventHooks::s_origPkNotify = nullptr;
void *EventHooks::s_origSpentGold = nullptr;
void *EventHooks::s_origCombatMsg = nullptr;

EventHooks &EventHooks::getInstance() {
  static EventHooks instance;
  return instance;
}

bool EventHooks::install() {
  if (m_installed)
    return true;

  // Initialize MinHook
  if (MH_Initialize() != MH_OK) {
    return false;
  }

  // Get module base (game may be rebased due to ASLR)
  HMODULE gameModule = GetModuleHandle(nullptr);
  DWORD_PTR moduleBase = (DWORD_PTR)gameModule;

  // Calculate actual addresses (handle ASLR rebasing)
  // The VAs assume base 0x00400000
  // Actual address = moduleBase + (VA - 0x00400000)

  // Create hook for ExpNotify
  void *expNotifyAddr = (void *)(moduleBase + (EXP_NOTIFY_VA - 0x00400000));
  if (MH_CreateHook(expNotifyAddr, (LPVOID)&HookedExpNotify,
                    reinterpret_cast<LPVOID *>(&g_origExpNotify)) == MH_OK) {
    MH_EnableHook(expNotifyAddr);
    s_origExpNotify = (void *)g_origExpNotify;
  }

  // Create hook for ItemNotify
  void *itemNotifyAddr = (void *)(moduleBase + (ITEM_NOTIFY_VA - 0x00400000));
  if (MH_CreateHook(itemNotifyAddr, (LPVOID)&HookedItemNotify,
                    reinterpret_cast<LPVOID *>(&g_origNotifyItemAdd)) ==
      MH_OK) {
    MH_EnableHook(itemNotifyAddr);
    s_origNotifyItemAdd = (void *)g_origNotifyItemAdd;
  }

  // Create hook for GameChat::recvMsg - for chat filtering
  void *recvMsgAddr = (void *)(moduleBase + (RECVMSG_VA - 0x00400000));
  MH_STATUS recvMsgStatus =
      MH_CreateHook(recvMsgAddr, (LPVOID)&HookedRecvMsg,
                    reinterpret_cast<LPVOID *>(&g_origRecvMsg));
  if (recvMsgStatus == MH_OK) {
    MH_EnableHook(recvMsgAddr);
    sprintf_s(g_debugText, sizeof(g_debugText), "RecvMsg hook OK at %p",
              recvMsgAddr);
  } else {
    sprintf_s(g_debugText, sizeof(g_debugText), "RecvMsg hook FAILED: %d at %p",
              recvMsgStatus, recvMsgAddr);
  }

  // Create hook for CombatMsg - for DPS tracking
  void *combatMsgAddr = (void *)(moduleBase + (COMBAT_MSG_VA - 0x00400000));
  if (MH_CreateHook(combatMsgAddr, (LPVOID)&HookedCombatMsg,
                    reinterpret_cast<LPVOID *>(&g_origCombatMsg)) == MH_OK) {
    MH_EnableHook(combatMsgAddr);
    s_origCombatMsg = (void *)g_origCombatMsg;
  }

  m_installed = true;
  return true;
}

void EventHooks::uninstall() {
  if (!m_installed)
    return;

  // Disable and remove all hooks
  MH_DisableHook(MH_ALL_HOOKS);
  MH_Uninitialize();

  m_installed = false;
}

// Legacy hook functions (kept for compatibility)
void __fastcall EventHooks::HookExpNotify(void *game, void *edx, void *data) {
  // Handled by HookedExpNotify above
}

void __fastcall EventHooks::HookNotifyItemAdd(void *game, void *edx,
                                              void *data) {
  // TODO: Implement item notification hook
}

//=============================================================================
// OverlayRenderer Implementation - Uses game's rendering!
//=============================================================================
void *OverlayRenderer::s_origWorldRender = nullptr;

OverlayRenderer &OverlayRenderer::getInstance() {
  static OverlayRenderer instance;
  return instance;
}

bool OverlayRenderer::initialize() {
  // Hook World::render so we can draw after the game renders
  void *renderAddr = ScanPattern(PATTERN_WORLD_RENDER);
  if (renderAddr) {
    s_origWorldRender = renderAddr;
    // Install hook...
  }
  return true;
}

void __fastcall OverlayRenderer::HookWorldRender(void *world, void *edx) {
  // Call original game render
  auto orig = (void(__fastcall *)(void *, void *))s_origWorldRender;
  orig(world, edx);

  // Now draw our overlay on top
  getInstance().render();
}

void OverlayRenderer::render() {
  // We're inside the game's render loop now!
  // We have access to the game's SFML window and can draw directly

  if (m_showStats)
    renderStatsPanel();
  if (m_showLoot)
    renderLootHistory();
  if (m_showKills)
    renderKillHistory();
}

void OverlayRenderer::renderStatsPanel() {}

void OverlayRenderer::renderLootHistory() {
  // Similar SFML drawing for loot list
}

void OverlayRenderer::renderKillHistory() {
  // Similar SFML drawing for kill list
}

void OverlayRenderer::shutdown() {
  // Cleanup rendering resources
}

void OverlayRenderer::showStatsPanel(bool show) { m_showStats = show; }

void OverlayRenderer::showLootPanel(bool show) { m_showLoot = show; }

void OverlayRenderer::showKillPanel(bool show) { m_showKills = show; }

void OverlayRenderer::updateStats(const CombatStats &player,
                                  const CombatStats &party) {
  m_playerStats = player;
  m_partyStats = party;
}

void OverlayRenderer::addLootEntry(const LootEntry &entry) {
  m_lootHistory.insert(m_lootHistory.begin(), entry);
  if (m_lootHistory.size() > 50) {
    m_lootHistory.pop_back();
  }
}

void OverlayRenderer::addKillEntry(const KillEntry &entry) {
  m_killHistory.insert(m_killHistory.begin(), entry);
  if (m_killHistory.size() > 50) {
    m_killHistory.pop_back();
  }
}

//=============================================================================
// Tracker Implementation
//=============================================================================
Tracker &Tracker::getInstance() {
  static Tracker instance;
  return instance;
}

bool Tracker::initialize() {
  if (m_initialized)
    return true;

  // Initialize shared memory for external GUI FIRST (so GUI can connect even if
  // hooks fail)
  if (!initSharedMemory()) {
    // Non-fatal - GUI just won't work, but log it
  }

  m_playerStats.reset();
  m_partyStats.reset();
  updateSharedMemory();

  // Initialize game bridge (may fail if patterns don't match)
  if (!GameBridge::getInstance().initialize()) {
    // Continue anyway - shared memory is up for GUI
  }

  // Set global instance for hooks
  g_trackerInstance = this;

  // Set up event hooks
  auto &hooks = EventHooks::getInstance();

  hooks.onExpGained = [this](int amount) { onExpGained(amount); };

  hooks.onLootReceived = [this](const LootEntry &loot) {
    onLootReceived(loot);
  };

  hooks.onMobKilled = [this](const std::string &name, int exp) {
    onMobKilled(name, exp);
  };

  // Hooks may fail if patterns don't match - that's ok
  hooks.install();

  // Initialize overlay (may fail)
  OverlayRenderer::getInstance().initialize();

  m_initialized = true;
  updateSharedMemory();

  return true;
}

void Tracker::shutdown() {
  if (!m_initialized)
    return;

  m_initialized = false; // Signal thread to stop

  // Wait for test thread to finish
  if (m_testThread.joinable()) {
    m_testThread.join();
  }

  g_trackerInstance = nullptr;

  EventHooks::getInstance().uninstall();
  OverlayRenderer::getInstance().shutdown();
  cleanupSharedMemory();
}

void Tracker::notifyExpGained(int amount) { onExpGained(amount); }

void Tracker::notifyMobKilled(const std::string &name, int exp) {
  onMobKilled(name, exp);
}

void Tracker::notifyLootReceived(const LootEntry &loot) {
  onLootReceived(loot);
}

void Tracker::notifyGoldChanged(int amount) {
  // Accumulate gold from chat parsing
  if (amount > 0) {
    m_playerStats.totalGold += amount;
    updateSharedMemory();
  }
}

void Tracker::notifyGoldSpent(int amount) {
  // Track gold spent (repair costs, purchases, etc.)
  if (amount > 0) {
    m_playerStats.goldSpent += amount;
    updateSharedMemory();
  }
}

void Tracker::notifyDamageDealt(int amount) {
  // Track damage dealt for DPS calculation
  if (amount > 0) {
    m_playerStats.totalDamage += amount;
    updateSharedMemory();
  }
}

void Tracker::onMobKilled(const std::string &name, int exp) {
  KillEntry entry;
  entry.mobName = name;
  entry.expGained = exp;
  entry.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
  entry.isPartyKill = GameBridge::getInstance().isInParty();

  m_killHistory.push_back(entry);
  m_playerStats.totalKills++;

  if (entry.isPartyKill) {
    m_partyStats.totalKills++;
  }

  OverlayRenderer::getInstance().addKillEntry(entry);
  OverlayRenderer::getInstance().updateStats(m_playerStats, m_partyStats);
  updateSharedMemory();
}

void Tracker::onLootReceived(const LootEntry &loot) {
  m_lootHistory.push_back(loot);
  m_playerStats.totalLootItems += loot.amount;
  m_playerStats.lootByQuality[loot.quality] += loot.amount;

  // Check if gold
  constexpr uint16_t GOLD_ITEM = 1;
  if (loot.item.m_itemId == GOLD_ITEM) {
    m_playerStats.totalGold += loot.amount;
  }

  OverlayRenderer::getInstance().addLootEntry(loot);
  OverlayRenderer::getInstance().updateStats(m_playerStats, m_partyStats);
  updateSharedMemory();
}

void Tracker::onExpGained(int amount) {
  m_playerStats.totalExp += amount;
  OverlayRenderer::getInstance().updateStats(m_playerStats, m_partyStats);
  updateSharedMemory();
}

void Tracker::resetStats() {
  m_playerStats.reset();
  m_partyStats.reset();
  m_lootHistory.clear();
  m_killHistory.clear();
  OverlayRenderer::getInstance().updateStats(m_playerStats, m_partyStats);
  updateSharedMemory();
}

void Tracker::toggleOverlay() {
  m_overlayVisible = !m_overlayVisible;
  OverlayRenderer::getInstance().showStatsPanel(m_overlayVisible);
  OverlayRenderer::getInstance().showLootPanel(m_overlayVisible);
  OverlayRenderer::getInstance().showKillPanel(m_overlayVisible);
  updateSharedMemory();
}

bool Tracker::initSharedMemory() {
  // Create mutex for synchronization
  m_mutexHandle = CreateMutexA(nullptr, FALSE, TRACKER_MUTEX_NAME);
  if (!m_mutexHandle) {
    return false;
  }

  // Create shared memory
  m_sharedMemHandle =
      CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                         sizeof(SharedTrackerData), TRACKER_SHARED_MEMORY_NAME);

  if (!m_sharedMemHandle) {
    CloseHandle(m_mutexHandle);
    m_mutexHandle = nullptr;
    return false;
  }

  // Map the shared memory
  m_sharedData = static_cast<SharedTrackerData *>(MapViewOfFile(
      m_sharedMemHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedTrackerData)));

  if (!m_sharedData) {
    CloseHandle(m_sharedMemHandle);
    CloseHandle(m_mutexHandle);
    m_sharedMemHandle = nullptr;
    m_mutexHandle = nullptr;
    return false;
  }

  // Initialize shared data
  ZeroMemory(m_sharedData, sizeof(SharedTrackerData));
  m_sharedData->magic = 0xDEADBEEF;
  m_sharedData->overlayVisible = true;
  m_sharedData->sessionStartTime =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();

  // Set global pointer for chat filter access
  g_sharedData = m_sharedData;

  return true;
}

void Tracker::cleanupSharedMemory() {
  if (m_sharedData) {
    UnmapViewOfFile(m_sharedData);
    m_sharedData = nullptr;
  }
  if (m_sharedMemHandle) {
    CloseHandle(m_sharedMemHandle);
    m_sharedMemHandle = nullptr;
  }
  if (m_mutexHandle) {
    CloseHandle(m_mutexHandle);
    m_mutexHandle = nullptr;
  }
}

void Tracker::updateSharedMemory() {
  if (!m_sharedData || !m_mutexHandle)
    return;

  // Acquire mutex
  WaitForSingleObject(m_mutexHandle, 100);

  // Update player stats
  m_sharedData->totalKills = m_playerStats.totalKills;
  m_sharedData->totalLootItems = m_playerStats.totalLootItems;
  m_sharedData->totalGold = m_playerStats.totalGold;
  m_sharedData->totalExp = m_playerStats.totalExp;
  m_sharedData->goldSpent = m_playerStats.goldSpent;
  m_sharedData->totalDamage = m_playerStats.totalDamage;

  // Update party stats
  m_sharedData->partyKills = m_partyStats.totalKills;
  m_sharedData->partyLootItems = m_partyStats.totalLootItems;
  m_sharedData->partyGold = m_partyStats.totalGold;
  m_sharedData->partyExp = m_partyStats.totalExp;

  // Update loot by quality
  for (int i = 0; i < 6; i++) {
    auto it = m_playerStats.lootByQuality.find(static_cast<ItemQuality>(i));
    m_sharedData->lootByQuality[i] =
        (it != m_playerStats.lootByQuality.end()) ? it->second : 0;
  }

  m_sharedData->overlayVisible = m_overlayVisible;

  // Copy debug text (combine regular and combat debug)
  extern char g_debugText[512];
  extern char g_combatDebug[512];
  _snprintf_s(m_sharedData->debugText, sizeof(m_sharedData->debugText),
              _TRUNCATE, "%s\n%s", g_debugText, g_combatDebug);

  // Sync recent loot entries (last 10)
  int lootCount = (int)m_lootHistory.size();
  m_sharedData->recentLootIndex = lootCount % 10;
  for (int i = 0; i < 10; i++) {
    if (i < lootCount) {
      int srcIdx = (lootCount > 10) ? (lootCount - 10 + i) : i;
      const LootEntry &src = m_lootHistory[srcIdx];
      strncpy(m_sharedData->recentLoot[i].itemName, src.itemName.c_str(), 63);
      m_sharedData->recentLoot[i].itemName[63] = '\0';
      m_sharedData->recentLoot[i].quality = (uint8_t)src.quality;
      m_sharedData->recentLoot[i].amount = src.amount;
      m_sharedData->recentLoot[i].timestamp = src.timestamp;
    } else {
      m_sharedData->recentLoot[i].itemName[0] = '\0';
    }
  }

  // Sync recent kills (last 10)
  int killCount = (int)m_killHistory.size();
  m_sharedData->recentKillIndex = killCount % 10;
  for (int i = 0; i < 10; i++) {
    if (i < killCount) {
      int srcIdx = (killCount > 10) ? (killCount - 10 + i) : i;
      const KillEntry &src = m_killHistory[srcIdx];
      strncpy(m_sharedData->recentKills[i].mobName, src.mobName.c_str(), 63);
      m_sharedData->recentKills[i].mobName[63] = '\0';
      m_sharedData->recentKills[i].expGained = src.expGained;
      m_sharedData->recentKills[i].timestamp = src.timestamp;
    } else {
      m_sharedData->recentKills[i].mobName[0] = '\0';
    }
  }

  // Release mutex
  ReleaseMutex(m_mutexHandle);
}

} // namespace DreadmystTracker

//=============================================================================
// DLL Entry Point
//=============================================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
  using namespace DreadmystTracker;

  switch (reason) {
  case DLL_PROCESS_ATTACH:
    DisableThreadLibraryCalls(hModule);
    // Initialize on a separate thread to not block game loading
    CreateThread(
        nullptr, 0,
        [](LPVOID) -> DWORD {
          Sleep(3000); // Wait for game to fully initialize
          Tracker::getInstance().initialize();
          return 0;
        },
        nullptr, 0, nullptr);
    break;

  case DLL_PROCESS_DETACH:
    Tracker::getInstance().shutdown();
    break;
  }
  return TRUE;
}

extern "C" __declspec(dllexport) void ToggleOverlay() {
  DreadmystTracker::Tracker::getInstance().toggleOverlay();
}

extern "C" __declspec(dllexport) void ResetStats() {
  DreadmystTracker::Tracker::getInstance().resetStats();
}
