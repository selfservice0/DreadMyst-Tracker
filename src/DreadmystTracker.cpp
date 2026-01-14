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

// Hook function for chat messages
void __fastcall HookedAddMessage(void *thisPtr, void *edx, const char *message,
                                 int color) {
  // Call original first so message displays
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
typedef void(__thiscall *OrigAddLine_t)(void *thisPtr, void *strBuf,
                                        int channel, void *linkedItem);

static OrigExpNotify_t g_origExpNotify = nullptr;
static OrigNotifyItemAdd_t g_origNotifyItemAdd = nullptr;
static OrigPkNotify_t g_origPkNotify = nullptr;
static OrigAddLine_t g_origAddLine = nullptr;

// Discovered function addresses from Dreadmyst.exe analysis:
// Game::processPacket_Server_ExpNotify at VA 0x0045E320
static constexpr DWORD EXP_NOTIFY_VA = 0x0045E320;
// Game::processPacket_Server_NotifyItemAdd at VA 0x004673C0
static constexpr DWORD ITEM_NOTIFY_VA = 0x004673C0;
// Game::processPacket_Server_PkNotify at VA 0x0045DE50
static constexpr DWORD PK_NOTIFY_VA = 0x0045DE50;
// Game::processPacket_Server_SpentGold at VA 0x0045EDD0
static constexpr DWORD GOLD_NOTIFY_VA = 0x0045EDD0;
// GameChat::addLine (FUN_00472ac0) - for parsing exp from chat strings
static constexpr DWORD ADDLINE_VA = 0x00472ac0;
// Game::processPacket_Server_CombatMsg at VA 0x00468110 - for DPS tracking
static constexpr DWORD COMBAT_MSG_VA = 0x00468110;
// GameChat::recvMsg (FUN_00471e60) - for chat filtering
static constexpr DWORD RECVMSG_VA = 0x00471e60;

// RecvMsg hook for chat filtering
typedef void(__thiscall *OrigRecvMsg_t)(void *thisPtr, void *msgStr,
                                        void *fromStr, int channel,
                                        void *linkedItem);
static OrigRecvMsg_t g_origRecvMsg = nullptr;

// Forward declaration for chat filter access
static SharedTrackerData *g_sharedData = nullptr;

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

// Combat message packet structure (simplified, based on game source)
// The packet contains: targetGuid, casterGuid, amount, spellId, etc.
// We read the amount from the packet data buffer
void __fastcall HookedCombatMsg(void *thisPtr, void *edx, void *data) {
  // Call original first
  if (g_origCombatMsg) {
    g_origCombatMsg(thisPtr, data);
  }

  // Parse damage from the packet data
  // The packet structure GP_Server_CombatMsg has m_amount at a certain offset
  // Based on source: m_targetGuid (4), m_casterGuid (4), m_amount (4), etc.
  // We check if we're the caster and damage is negative (dealt damage)
  if (data && g_trackerInstance) {
    // Packet layout (approximate from game source analysis):
    // Offset 0: targetGuid (int32)
    // Offset 4: casterGuid (int32)
    // Offset 8: amount (int32) - negative = damage, positive = heal
    // This is a simplification - actual offsets may differ
    int32_t *packetData = (int32_t *)data;
    int32_t amount = packetData[2]; // Approximate offset for m_amount

    // If amount is negative, it's damage dealt
    if (amount < 0) {
      int damage = -amount;
      g_trackerInstance->notifyDamageDealt(damage);
    }
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

// Hook for GameChat::addLine - parses exp from chat strings
void __fastcall HookedAddLine(void *thisPtr, void *edx, void *strBuf,
                              int channel, void *linkedItem) {
  // Call original first
  if (g_origAddLine) {
    g_origAddLine(thisPtr, strBuf, channel, linkedItem);
  }

  // Try to extract string from the std::string buffer
  // std::string layout: [ptr_or_sso][size][capacity] or SSO inline
  // For SSO: if capacity < 16, string is inline at start of object
  if (strBuf) {
    char *strPtr = nullptr;

    // SSO threshold is typically 15 chars. Check the capacity field.
    uint32_t *ssoBuf = (uint32_t *)strBuf;
    uint32_t capacity = ssoBuf[5]; // capacity is at offset 20 (5 * 4)

    if (capacity < 16) {
      // SSO: string is inline at the start of the buffer
      strPtr = (char *)strBuf;
    } else {
      // Heap allocated: first pointer is the string data
      strPtr = *(char **)strBuf;
    }

    if (strPtr && (uintptr_t)strPtr > 0x10000 &&
        (uintptr_t)strPtr < 0x7FFFFFFF) {
      // Check for exp message: "You gained X experience"
      if (strstr(strPtr, "You gained") && strstr(strPtr, "experience")) {
        // Parse the number: "You gained %d experience"
        int expAmount = 0;
        const char *numStart = strPtr + 11; // Skip "You gained "
        while (*numStart && (*numStart < '0' || *numStart > '9'))
          numStart++;
        while (*numStart >= '0' && *numStart <= '9') {
          expAmount = expAmount * 10 + (*numStart - '0');
          numStart++;
        }

        if (expAmount > 0 && g_trackerInstance) {
          g_trackerInstance->notifyExpGained(expAmount);
          sprintf(g_debugText, "Exp gained: %d\nTotal events: %d", expAmount,
                  g_expEventCount);
        }
      }

      // Check for loot message: "You receive: [ItemName]" or "[Player]
      // received: [ItemName]"
      const char *receivePos = strstr(strPtr, "receive: [");
      if (receivePos && g_trackerInstance) {
        // Find the brackets to extract item name
        const char *nameStart = strstr(receivePos, "[");
        const char *nameEnd = strstr(nameStart ? nameStart + 1 : nullptr, "]");

        if (nameStart && nameEnd && nameEnd > nameStart) {
          nameStart++; // Skip [
          char itemName[64] = {0};
          int len = (int)(nameEnd - nameStart);
          if (len > 63)
            len = 63;
          strncpy(itemName, nameStart, len);
          itemName[len] = '\0';

          // Check for amount " xN" after the ]
          int amount = 1;
          const char *amtPos = strstr(nameEnd, " x");
          if (amtPos) {
            amount = atoi(amtPos + 2);
            if (amount < 1)
              amount = 1;
          }

          // Check if it's gold
          if (strstr(itemName, "Gold") || strstr(itemName, "gold")) {
            g_trackerInstance->notifyGoldChanged(amount);
            sprintf(g_debugText, "Gold: +%d\nExp events: %d", amount,
                    g_expEventCount);
          } else {
            LootEntry entry;
            entry.item.m_itemId = 0;
            entry.itemName = itemName;
            entry.quality = ItemQuality::QualityLv1;
            entry.amount = amount;
            g_trackerInstance->notifyLootReceived(entry);
            sprintf(g_debugText, "Loot: %s x%d\nExp events: %d", itemName,
                    amount, g_expEventCount);
          }
        }
      }

      // Check for gold spent: "You spent X Gold"
      const char *spentPos = strstr(strPtr, "You spent ");
      if (spentPos && strstr(spentPos, " Gold") && g_trackerInstance) {
        int goldSpent = atoi(spentPos + 10); // Skip "You spent "
        if (goldSpent > 0) {
          g_trackerInstance->notifyGoldSpent(goldSpent);
          sprintf(g_debugText, "Gold spent: -%d\nExp: %d", goldSpent,
                  g_expEventCount);
        }
      }
    }
  }
}

// Hook for GameChat::recvMsg - filters chat messages before display
void __fastcall HookedRecvMsg(void *thisPtr, void *edx, void *msgStr,
                              void *fromStr, int channel, void *linkedItem) {
  bool shouldBlock = false;

  __try {
    if (g_sharedData && g_sharedData->chatFilterEnabled) {

      // Check for linked item blocking
      if (g_sharedData->blockLinkedItems && linkedItem != nullptr) {
        shouldBlock = true;
      }

      // Check filter terms if we have any and haven't already decided to block
      if (!shouldBlock && g_sharedData->chatFilterTerms[0] != '\0' && msgStr) {
        char *strPtr = nullptr;
        uint32_t *ssoBuf = (uint32_t *)msgStr;
        uint32_t capacity = ssoBuf[5];

        if (capacity < 16) {
          strPtr = (char *)msgStr;
        } else {
          strPtr = *(char **)msgStr;
        }

        if (strPtr && (uintptr_t)strPtr > 0x10000 &&
            (uintptr_t)strPtr < 0x7FFFFFFF) {

          // Also block if message contains item brackets and blockLinkedItems
          // is on
          if (g_sharedData->blockLinkedItems &&
              strchr(strPtr, '[') != nullptr) {
            shouldBlock = true;
          }

          // Check custom filter terms
          if (!shouldBlock) {
            char msgLower[512];
            strncpy_s(msgLower, sizeof(msgLower), strPtr, sizeof(msgLower) - 1);
            for (char *p = msgLower; *p; p++)
              *p = (char)tolower(*p);

            char filterCopy[512];
            strncpy_s(filterCopy, sizeof(filterCopy),
                      g_sharedData->chatFilterTerms, sizeof(filterCopy) - 1);

            char *context = nullptr;
            char *token = strtok_s(filterCopy, ",", &context);
            while (token != nullptr) {
              while (*token == ' ')
                token++;
              char *end = token + strlen(token) - 1;
              while (end > token && *end == ' ')
                *end-- = '\0';

              if (strlen(token) > 0) {
                char termLower[64];
                strncpy_s(termLower, sizeof(termLower), token,
                          sizeof(termLower) - 1);
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
      }

      // If blockLinkedItems is on but no filter terms, still check for brackets
      if (!shouldBlock && g_sharedData->blockLinkedItems &&
          g_sharedData->chatFilterTerms[0] == '\0' && msgStr) {
        char *strPtr = nullptr;
        uint32_t *ssoBuf = (uint32_t *)msgStr;
        uint32_t capacity = ssoBuf[5];

        if (capacity < 16) {
          strPtr = (char *)msgStr;
        } else {
          strPtr = *(char **)msgStr;
        }

        if (strPtr && (uintptr_t)strPtr > 0x10000 &&
            (uintptr_t)strPtr < 0x7FFFFFFF) {
          if (strchr(strPtr, '[') != nullptr) {
            shouldBlock = true;
          }
        }
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    shouldBlock = false;
  }

  if (shouldBlock) {
    return; // Don't call original - block message
  }

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

  // Create hook for GameChat::addLine - parses exp from chat strings
  void *addLineAddr = (void *)(moduleBase + (ADDLINE_VA - 0x00400000));
  if (MH_CreateHook(addLineAddr, (LPVOID)&HookedAddLine,
                    reinterpret_cast<LPVOID *>(&g_origAddLine)) == MH_OK) {
    MH_EnableHook(addLineAddr);
  }

  // Create hook for CombatMsg - for DPS tracking
  void *combatMsgAddr = (void *)(moduleBase + (COMBAT_MSG_VA - 0x00400000));
  if (MH_CreateHook(combatMsgAddr, (LPVOID)&HookedCombatMsg,
                    reinterpret_cast<LPVOID *>(&g_origCombatMsg)) == MH_OK) {
    MH_EnableHook(combatMsgAddr);
    s_origCombatMsg = (void *)g_origCombatMsg;
  }

  // Create hook for GameChat::recvMsg - for chat filtering
  void *recvMsgAddr = (void *)(moduleBase + (RECVMSG_VA - 0x00400000));
  if (MH_CreateHook(recvMsgAddr, (LPVOID)&HookedRecvMsg,
                    reinterpret_cast<LPVOID *>(&g_origRecvMsg)) == MH_OK) {
    MH_EnableHook(recvMsgAddr);
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

  // Copy debug text
  extern char g_debugText[512];
  memcpy(m_sharedData->debugText, g_debugText, sizeof(g_debugText));

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
