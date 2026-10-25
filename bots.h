//
// Bots: pooled characters that log in while real players are online.
//
//   Each bot is a real character with a session that never touches the network.
//   It acts by sending client packets through the normal handlers, so the server
//   treats it like any other player.
//

#pragma once

#include "common/cbasetypes.h"
#include "common/mmo.h"
#include "common/timer.h"
#include "data/enums/zone.h"

#include "map/entities/entity_id.h"
#include "map/map_session.h"
#include "map/lua/luautils.h"
#include "map/packets/basic.h"

#include <cctype>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

class CBaseEntity;
class CBattleEntity;
class CCharEntity;
class CPathFind;
class PacketSystem;
enum class SpellID : uint16;
namespace xi
{
enum class Job : uint8_t;
}

namespace bots
{

struct PoolEntry
{
    std::string name;
    uint32      charId{};
    uint32      accId{};
    uint8       job{};
    uint8       level{};
};

// A zone and a spot in it.
struct Place
{
    xi::ZoneId zone{};
    position_t pos{};
};

// A chat line a bot received.
struct Heard
{
    uint8       kind{}; // CHAT_MESSAGE_TYPE
    std::string from;
    std::string text;
};

// A request from a party member in chat, being carried out.
enum class OrderKind : uint8
{
    Cast,    // spells, highest tier first, on targets (players) or the speaker's monster
    Provoke, // the speaker's monster
    Attack,
    Stop,
    Rest,
    Stay,
    Follow,
    Pull,     // become the party's puller
    StopPull, // nobody pulls
    Camp,     // camp is where the leader stands now
    Assist,   // fight whatever the subject fights, from now on
};

struct Order
{
    OrderKind            kind{};
    std::vector<SpellID> spells;
    std::vector<uint32>  targets; // players to cast on; empty for the speaker's monster
    uint32               speakerId{};
    uint32               subject{}; // Follow and Assist: whom; 0 for the speaker
    timer::time_point    expires{};
};

struct PlayerZone
{
    uint8      level{}; // highest real player level in the zone
    position_t pos{};   // where that player is
};

// A nuke a bot cast, kept until its next turn to see how much enmity it really added.
struct NukeWatch
{
    EntityId          target;
    SpellID           spell{};
    int64             before{};   // the bot's enmity on the target when it cast
    int64             beforeVE{}; // the volatile part, which decays while the spell is cast
    int64             estimate{}; // what the spell's base and multiplier said it would add, unscaled
    timer::time_point castAt{};
};

struct Bot
{
    uint32                      charId{};
    uint32                      accId{};
    std::string                 name;
    uint8                       job{};
    std::unique_ptr<MapSession> session;
    timer::time_point           diedAt{};
    bool                        withPlayer{};    // its party has had a real player in it
    uint8                       preparedLevel{}; // level its spells and gear were set for
    position_t                  home{};          // where it logged in, for idle wandering
    timer::time_point           nextWander{};
    position_t                  leaderLast{};    // leader position last tick, to tell when they stop
    bool                        seeking{};       // the /sea seeking-party flag is on
    std::vector<Heard>          heard;           // chat received this tick, for bot_chat.cpp
    position_t                  settledAt{};     // where it was last put on the ground
    std::vector<std::string>    invites;         // people to invite, one per tick: players who asked, or bots it's grouping with
    std::vector<std::string>    pearlFor;        // players who asked it for a linkpearl
    uint32                      entranceId{};    // the battlefield entrance the leader last opened, to follow them in by
    uint16                      entranceTargid{};
    uint8                       entryTries{};    // entrance clicks since the leader went in; a full battlefield stops it
    bool                        entryAnswered{}; // the entry menu has its choice; next the event is ended
    timer::time_point           leaderMovedAt{};
    std::optional<Order>        order;           // a request from chat being carried out
    std::deque<Order>           queued;          // more requests from the same line, one after another
    bool                        stay{};          // asked to stay put: fights, but doesn't follow
    bool                        restNow{};       // asked to rest
    uint32                      holdTarget{};    // asked to stop attacking this monster
    uint32                      focusTarget{};   // asked to attack this monster
    uint32                      followId{};      // told to follow this party member instead of the leader
    uint32                      assistId{};      // told to fight this party member's target instead of the leader's
    timer::time_point           foughtAt{};      // last tick it was in a fight
    timer::time_point           playerGoneAt{};  // when its party last had no real player in it
    timer::time_point           nextChatter{};   // when it may next start a conversation
    timer::time_point           nextHunt{};      // when a bot on its own next looks for something to fight
    bool                        sitting{};       // /sit in town
    uint16                      ammoId{};        // ammunition its gear plan picked, restocked when it runs out
    uint8                       emoteBack{};     // an emote to answer a player's with, once emoteAt comes
    uint32                      emoteTo{};
    timer::time_point           emoteAt{};
    std::map<uint64, timer::time_point> attempts; // per action and target, the earliest retry
    std::optional<NukeWatch>    nukeWatch;       // the last nuke, until the enmity it added is known
    std::deque<float>           nukeRatios;      // enmity added over estimated, for the last few nukes

    auto entity() const -> CCharEntity*;
};

// Medicines every bot carries, restocked like ammunition.
constexpr uint16 kEchoDrops   = 4151;
constexpr uint16 kPrismPowder = 4164;
constexpr uint16 kSilentOil   = 4165;

// Bots rest below these, and pullers wait for everyone to be above them.
constexpr uint8 kRestHPP = 70;
constexpr uint8 kRestMPP = 60;
// The party's healer needs this much MP before another monster comes in.
constexpr uint8 kHealerPullMPP = 85;

// Lowercase, for matching names.
inline auto lowerCase(std::string text) -> std::string
{
    for (auto& c : text)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

// bot_pool.cpp
void ensureSchema();
auto loadPool() -> std::vector<PoolEntry>;
void clearStaleState();
auto countRealPlayers() -> uint32;
void registerPoolChar(uint32 charId);
auto isPoolChar(uint32 charId) -> bool;

// bot_ai.cpp
void think(Bot& bot, PacketSystem& packets, const std::set<uint32>& botIds, bool active, timer::time_point tick);
void send(Bot& bot, PacketSystem& packets, CBasicPacket& packet);
void leaveParty(Bot& bot, PacketSystem& packets);
auto inspect(const Bot& bot) -> std::vector<std::string>;
void castSpell(Bot& bot, PacketSystem& packets, SpellID spellId, const CBaseEntity* PTarget);
void useAbility(Bot& bot, PacketSystem& packets, uint16 abilityId, const CBaseEntity* PTarget);
void weaponSkill(Bot& bot, PacketSystem& packets, uint16 skillId, const CBaseEntity* PTarget);
void rangedAttack(Bot& bot, PacketSystem& packets, const CBaseEntity* PTarget);
auto useItem(Bot& bot, PacketSystem& packets, uint16 itemId, const CBaseEntity* PTarget) -> bool;
void engage(Bot& bot, PacketSystem& packets, const CBaseEntity* PTarget);
void disengage(Bot& bot, PacketSystem& packets);
void walkTo(CCharEntity* PChar, const position_t& pos, float within, timer::time_point tick);

// bot_combat.cpp
auto throttle(Bot& bot, uint32 key, uint32 targetId, timer::duration cooldown, timer::time_point tick) -> bool;
auto isCaster(xi::Job job) -> bool;
auto isRanged(CCharEntity* PChar) -> bool;
auto mainTank(CCharEntity* PChar) -> CBattleEntity*;
auto pulledHate(CCharEntity* PChar, CBattleEntity* PTarget) -> bool;
auto chasedOffTank(CCharEntity* PChar) -> bool;
auto healingWanted(CCharEntity* PChar) -> bool;
auto partyHealer(CCharEntity* PChar) -> CBattleEntity*;
auto hateStatus(CCharEntity* PChar, CBattleEntity* PTarget) -> std::string;
auto deadToRaise(CCharEntity* PChar) -> CCharEntity*;
auto raiserNearby(CCharEntity* PChar) -> bool;
auto pullRange(CCharEntity* PChar) -> float;
auto openPull(Bot& bot, PacketSystem& packets, CBattleEntity* PMob, timer::time_point tick) -> bool;
auto prettyName(const std::string& name) -> std::string;
auto sleepLink(Bot& bot, PacketSystem& packets, CBattleEntity* PMob, const CBattleEntity* PBesides, timer::time_point tick) -> bool;
auto speedUp(Bot& bot, PacketSystem& packets, timer::time_point tick) -> bool;
auto asleep(const CBattleEntity* PEntity) -> bool;
auto addNeedsSleep(CCharEntity* PChar, CBattleEntity* PTarget) -> bool;
auto conceal(Bot& bot, PacketSystem& packets, const CCharEntity* PGuide, timer::time_point tick) -> bool;

// bot_pull.cpp
void setPuller(CBattleEntity* PLeader, uint32 pullerId);
void stopPulling(CBattleEntity* PLeader);
void setCamp(CBattleEntity* PLeader);
void breakCamp(CBattleEntity* PLeader);
auto campSpot(const CCharEntity* PChar) -> std::optional<position_t>;
auto beingPulled(const CCharEntity* PChar, const CBattleEntity* PMob) -> bool;
auto pull(Bot& bot, PacketSystem& packets, CCharEntity* PChar, CCharEntity* PLeader, timer::time_point tick) -> bool;
auto pullStatus(const CCharEntity* PChar) -> std::string;
auto autoCamp(CCharEntity* PLeader, bool withPlayer) -> bool;
auto canAct(CCharEntity* PChar) -> bool;
auto act(Bot& bot, PacketSystem& packets, CBattleEntity* PTarget, bool travelling, timer::time_point tick) -> bool;
auto followOrder(Bot& bot, PacketSystem& packets, timer::time_point tick) -> bool;

void travel(CCharEntity* PChar, xi::ZoneId zone, const position_t& pos);

// bot_move.cpp
auto makePathFind(CCharEntity* PChar) -> std::unique_ptr<CPathFind>;
void settle(Bot& bot);
auto combatSpot(CCharEntity* PChar, CBattleEntity* PTarget, float range) -> std::optional<position_t>;

// bot_world.cpp
auto findCamps(uint8 level) -> std::vector<Place>;
auto campZoneNames(uint8 level, std::size_t count) -> std::vector<std::string>;
auto spotNear(xi::ZoneId zone, const position_t& around, float minRadius, float maxRadius) -> position_t;
void placeAt(uint32 charId, const Place& place);
void placeAtHome(uint32 charId);
auto playerZones(const std::set<uint32>& botIds) -> std::map<xi::ZoneId, PlayerZone>;
auto occupiedZones(const std::set<uint32>& botIds) -> std::set<xi::ZoneId>;

// bot_http.cpp
struct HttpResult
{
    int         status{}; // 0 when there was no HTTP answer at all
    std::string body;     // the reply body, or what went wrong
};
auto httpPost(const std::string& url, const std::string& apiKey, const std::string& body, int timeoutSeconds) -> HttpResult;

// bot_orders.cpp
struct OrderPlan
{
    Bot*                 bot{};
    std::optional<Order> order; // none when the bot can't do it
    std::string          note;  // what it was asked and whether it's doing it, for its chat reply
    bool                 quiet{}; // one of several bots given the same job: only the first answers
};
auto planOrder(const std::string& text, CCharEntity* PSpeaker, const std::vector<Bot*>& bots) -> std::vector<OrderPlan>;
auto orderSubject(const std::string& text, OrderKind kind, CCharEntity* PSpeaker) -> CBattleEntity*;
auto pullerRank(xi::Job job, uint8 level) -> int;

// bot_brain.cpp
struct BrainOutcome
{
    uint64                 id{};
    bool                   answered{}; // false when the model didn't answer: fall back to the keyword parser
    std::vector<OrderPlan> plans;      // empty when it decided there was nothing to do
};
auto askBrain(const std::string& text, CCharEntity* PSpeaker, const std::vector<Bot*>& bots) -> uint64;
auto brainOutcomes(std::vector<Bot>& online) -> std::vector<BrainOutcome>;

// bot_chat.cpp
void startChat();
void handleChat(std::vector<Bot>& online, const std::set<std::string>& botNames);
void chatter(std::vector<Bot>& online, const std::set<std::string>& botNames, timer::time_point tick);
void deliverChat(std::vector<Bot>& online, PacketSystem& packets);
void callout(const Bot& bot, const std::string& text, bool macro);
void tell(const Bot& bot, const std::string& to, const std::string& text);
void greet(const Bot& bot);

// bot_linkshell.cpp
void ensureLinkshell();
auto offersPearls(const Bot& bot) -> bool;
auto linkshellPlayers() -> std::vector<std::string>;
void linkshellUpkeep(Bot& bot, sol::state& lua, timer::time_point tick);

// bot_gear.cpp
void prepare(Bot& bot, sol::state& lua);
void restock(Bot& bot, sol::state& lua, timer::time_point tick);

template <typename T>
auto makePacket() -> std::unique_ptr<CBasicPacket>
{
    auto packet = std::make_unique<CBasicPacket>();
    packet->setType(static_cast<uint16>(T::packetId));
    packet->setSize(sizeof(T));
    return packet;
}

} // namespace bots
