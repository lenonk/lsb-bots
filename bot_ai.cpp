//
// Bot behaviour, run once per logic tick for each online bot.
//

#include "bots.h"

#include "common/settings.h"
#include "common/utils.h"
#include "common/xirand.h"

#include "map/ai/ai_container.h"
#include "map/ai/helpers/pathfind.h"
#include "map/entities/char_entity.h"
#include "map/entities/mob_entity.h"
#include "map/entities/npc_entity.h"
#include "map/enums/party_kind.h"
#include "map/event_info.h"
#include "map/notoriety_container.h"
#include "map/packet_system.h"
#include "map/packets/c2s/0x01a_action.h"
#include "map/packets/c2s/0x037_item_use.h"
#include "map/battlefield.h"
#include "map/packets/c2s/0x05b_eventend.h"
#include "map/packets/c2s/0x05c_eventendxzy.h"
#include "map/packets/c2s/0x06e_group_solicit_req.h"
#include "map/packets/c2s/0x06f_group_leave.h"
#include "map/packets/c2s/0x070_group_breakup.h"
#include "map/packets/c2s/0x074_group_solicit_res.h"
#include "map/packets/c2s/0x0dc_config.h"
#include "data/enums/emote.h"
#include "data/enums/emote_mode.h"
#include "map/packets/c2s/0x05d_motion.h"
#include "map/packets/c2s/0x0e8_camp.h"
#include "map/packets/c2s/0x0ea_sit.h"
#include "map/packets/s2c/0x017_chat_std.h"
#include "map/packets/s2c/0x05a_motionmes.h"
#include "map/packets/s2c/0x11d_partyreq.h"
#include "map/party.h"
#include "map/spell.h"
#include "map/status_effect_container.h"
#include "map/utils/zoneutils.h"
#include "map/zone.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cctype>
#include <cstring>
#include <string_view>
#include <cstddef>
#include <optional>
#include <utility>

namespace bots
{

namespace
{

constexpr float kFollowDistance  = 3.0f;
constexpr float kRegroupDistance = 50.0f; // further than this from the leader, warp to them
constexpr float kCasterDistance  = 15.0f; // casters keep about this far from their target
constexpr float kArcherDistance  = 9.0f;  // close enough for full ranged damage
constexpr float kToTankDistance  = 4.0f;  // how close to the tank a bot runs when it pulls hate
constexpr float kRaiseRange      = 18.0f; // a little inside casting range
constexpr float kCampSpread      = 6.0f;  // how far from the camp spot bots stand
constexpr float kEmoteNear       = 6.0f;  // an emote at nobody in particular is answered this close
constexpr uint32 kEmoteChance    = 60;    // percent, for those
constexpr float kCampHold        = 10.0f; // a monster further out than this from camp is waited for
constexpr uint8 kHelpBelowHPP    = 40;    // unless whoever it's on is below this
constexpr uint8 kFightRestMPP    = 30;    // a WHM or BLM below this rests mid-fight
constexpr uint8 kFightResumeMPP  = 50;    // and a BLM gets back up to nuke at this
constexpr float kWanderRadius    = 6.0f;
constexpr float kThreatRange     = 40.0f; // how far away a monster attacking the party gets noticed
constexpr float kHuntRange       = 30.0f; // how far a bot on its own looks for something to fight
constexpr auto  kHomepointDelay  = std::chrono::seconds(60);
constexpr auto  kRaiseWait       = std::chrono::minutes(3);
constexpr auto  kSettleTime      = std::chrono::seconds(2); // the leader has stopped for this long
constexpr auto  kPlayerGoneGrace = std::chrono::seconds(20);
constexpr uint32 kAttackKey      = 0x30000;
constexpr uint32 kOutOfMPKey     = 0xB0000; // the mid-fight rest log, once a fight

void sendAction(Bot& bot, PacketSystem& packets, const GP_CLI_COMMAND_ACTION_ACTIONID actionId, const CBaseEntity* PTarget, const uint32 param = 0)
{
    auto  packet     = makePacket<GP_CLI_COMMAND_ACTION>();
    auto* action     = packet->as<GP_CLI_COMMAND_ACTION>();
    action->UniqueNo = PTarget->id;
    action->ActIndex = PTarget->targid;
    action->ActionID = actionId;
    action->ActionBuf[0] = param;
    send(bot, packets, *packet);
}

void acceptInvite(Bot& bot, PacketSystem& packets)
{
    auto packet                                         = makePacket<GP_CLI_COMMAND_GROUP_SOLICIT_RES>();
    packet->as<GP_CLI_COMMAND_GROUP_SOLICIT_RES>()->Res = static_cast<uint8_t>(GP_CLI_COMMAND_GROUP_SOLICIT_RES_RES::Accept);
    send(bot, packets, *packet);
}

// Ends a cutscene with its default choice, as a client skipping through it would.
void finishEvent(Bot& bot, PacketSystem& packets, CCharEntity* PChar)
{
    const auto* PTarget = PChar->currentEvent->targetEntity;

    auto  packet   = makePacket<GP_CLI_COMMAND_EVENTEND>();
    auto* end      = packet->as<GP_CLI_COMMAND_EVENTEND>();
    end->UniqueNo  = PTarget ? PTarget->id : PChar->id;
    end->ActIndex  = PTarget ? PTarget->targid : PChar->targid;
    end->Mode      = static_cast<uint16_t>(GP_CLI_COMMAND_EVENTEND_MODE::End);
    end->EventNum  = static_cast<uint16_t>(PChar->getZone());
    end->EventPara = static_cast<uint16_t>(PChar->currentEvent->eventId);
    send(bot, packets, *packet);
}

// Answers a join request (the newer /prcmd system) with a normal invite.
void invite(Bot& bot, PacketSystem& packets, const uint32 charId, const uint16 targId)
{
    auto  packet      = makePacket<GP_CLI_COMMAND_GROUP_SOLICIT_REQ>();
    auto* request     = packet->as<GP_CLI_COMMAND_GROUP_SOLICIT_REQ>();
    request->UniqueNo = charId;
    request->ActIndex = targId;
    request->Kind     = PartyKind::Party;
    send(bot, packets, *packet);
}

// A leader breaking up takes the whole party with it; leaving passes the lead on.
void leaveOrBreakUp(Bot& bot, PacketSystem& packets, const bool breakUp)
{
    auto* PChar = bot.entity();
    if (breakUp && PChar->PParty->GetLeader() == PChar)
    {
        auto packet                                      = makePacket<GP_CLI_COMMAND_GROUP_BREAKUP>();
        packet->as<GP_CLI_COMMAND_GROUP_BREAKUP>()->Kind = PartyKind::Party;
        send(bot, packets, *packet);
    }
    else
    {
        auto packet                                    = makePacket<GP_CLI_COMMAND_GROUP_LEAVE>();
        packet->as<GP_CLI_COMMAND_GROUP_LEAVE>()->Kind = PartyKind::Party;
        send(bot, packets, *packet);
    }
}

// The /sea "seeking party" flag, on while the bot has no party.
void setSeeking(Bot& bot, PacketSystem& packets, const bool seeking)
{
    auto  packet      = makePacket<GP_CLI_COMMAND_CONFIG>();
    auto* config      = packet->as<GP_CLI_COMMAND_CONFIG>();
    config->InviteFlg = 1;
    config->SetFlg    = static_cast<uint8_t>(seeking ? GP_CLI_COMMAND_CONFIG_SETFLG::On : GP_CLI_COMMAND_CONFIG_SETFLG::Off);
    send(bot, packets, *packet);
    bot.seeking = seeking;
}

void setResting(Bot& bot, PacketSystem& packets, const bool resting)
{
    auto packet                                = makePacket<GP_CLI_COMMAND_CAMP>();
    packet->as<GP_CLI_COMMAND_CAMP>()->Mode    = static_cast<uint32_t>(resting ? GP_CLI_COMMAND_CAMP_MODE::On : GP_CLI_COMMAND_CAMP_MODE::Off);
    send(bot, packets, *packet);
}

auto hasRealPlayer(const CParty* PParty, const std::set<uint32>& botIds) -> bool
{
    return std::ranges::any_of(PParty->members,
                               [&](const CBattleEntity* PMember)
                               {
                                   return PMember->objtype == TYPE_PC && !botIds.contains(PMember->id);
                               });
}

auto leaderOf(CCharEntity* PChar) -> CCharEntity*
{
    if (PChar->PParty == nullptr)
    {
        return nullptr;
    }

    auto* PLeader = dynamic_cast<CCharEntity*>(PChar->PParty->GetLeader());
    return PLeader == PChar ? nullptr : PLeader;
}

// The party member a bot was told to follow or assist, or the leader. A member zoning drops off the list for a moment, so
// the choice is only forgotten once the bot is out of the party.
auto partner(CCharEntity* PChar, uint32& id, CCharEntity* PLeader) -> CCharEntity*
{
    if (PChar->PParty == nullptr)
    {
        id = 0;
    }
    if (id == 0)
    {
        return PLeader;
    }
    for (auto* PMember : PChar->PParty->members)
    {
        if (PMember->id == id)
        {
            return dynamic_cast<CCharEntity*>(PMember);
        }
    }
    return PLeader;
}

auto inParty(const CCharEntity* PChar, const CBattleEntity* PEntity) -> bool
{
    if (PEntity == PChar)
    {
        return true;
    }
    return PChar->PParty != nullptr && std::ranges::contains(PChar->PParty->members, PEntity);
}

// The leader's target, or else the nearest mob attacking anyone in the party.
auto combatTarget(Bot& bot, CCharEntity* PChar, CCharEntity* PLeader) -> CBattleEntity*
{
    // Asked to leave this one alone.
    const auto valid = [&](CBattleEntity* PMob)
    {
        return PMob != nullptr && PMob->objtype == TYPE_MOB && !PMob->isDead() && PMob->getZone() == PChar->getZone() && PMob->id != bot.holdTarget;
    };

    // Asked to attack this one; a monster's target index is the low 12 bits of its id.
    if (bot.focusTarget != 0)
    {
        auto* PFocus = dynamic_cast<CBattleEntity*>(PChar->GetEntity(static_cast<uint16>(bot.focusTarget & 0xFFF), TYPE_MOB));
        if (PFocus != nullptr && PFocus->id == bot.focusTarget && valid(PFocus))
        {
            return PFocus;
        }
        bot.focusTarget = 0;
    }

    if (PLeader != nullptr && PLeader->PAI->IsEngaged() && valid(PLeader->GetBattleTarget()))
    {
        return PLeader->GetBattleTarget();
    }

    // A monster that aggros engages before anyone is on its hate list, so look for whatever is attacking the party.
    // A sleeping link waits until nothing awake is left.
    CBattleEntity* PNearest = nullptr;
    PChar->loc.zone->ForEachMob(
        [&](CMobEntity* PMob)
        {
            if (!valid(PMob) || !PMob->PAI->IsEngaged() || distance(PChar->loc.p, PMob->loc.p) >= kThreatRange || !inParty(PChar, PMob->GetBattleTarget()) ||
                beingPulled(PChar, PMob))
            {
                return;
            }
            const bool better = PNearest == nullptr || (asleep(PNearest) && !asleep(PMob)) ||
                                (asleep(PNearest) == asleep(PMob) && distance(PChar->loc.p, PMob->loc.p) < distance(PChar->loc.p, PNearest->loc.p));
            if (better)
            {
                PNearest = PMob;
            }
        });
    return PNearest;
}

void moveToward(CCharEntity* PChar, const position_t& pos, const float within, const timer::time_point tick)
{
    auto& path = *PChar->PAI->PathFind;

    if (distance(PChar->loc.p, pos) > within)
    {
        if (!path.IsFollowingPath() || distance(path.GetDestination(), pos) > within)
        {
            if (!path.PathAround(pos, within * 0.75f, PATHFLAG_RUN))
            {
                path.StepTo(pos, true);
            }
        }
    }
    else if (path.IsFollowingPath())
    {
        path.Clear();
    }

    if (path.IsFollowingPath())
    {
        path.FollowPath(tick);
    }
}

// Walks to an exact spot, repathing only when the spot has moved on (the target turned or moved).
void moveToSpot(CCharEntity* PChar, const position_t& spot, const timer::time_point tick)
{
    auto& path = *PChar->PAI->PathFind;

    if (distance(PChar->loc.p, spot) > 0.8f)
    {
        if ((!path.IsFollowingPath() || distance(path.GetDestination(), spot) > 1.0f) && !path.PathTo(spot, PATHFLAG_RUN))
        {
            path.StepTo(spot, true);
        }
    }
    else if (path.IsFollowingPath())
    {
        path.Clear();
    }

    if (path.IsFollowingPath())
    {
        path.FollowPath(tick);
    }
}

void follow(CCharEntity* PChar, CCharEntity* PLeader, const timer::time_point tick)
{
    if (PLeader->loc.zone == nullptr || PLeader->status != xi::Status::Normal || PLeader->inMogHouse())
    {
        return;
    }

    if (PLeader->getZone() != PChar->getZone())
    {
        travel(PChar, PLeader->getZone(), PLeader->loc.p);
        return;
    }

    if (distance(PChar->loc.p, PLeader->loc.p) > kRegroupDistance)
    {
        PChar->PAI->PathFind->WarpTo(PLeader->loc.p, kFollowDistance);
        return;
    }

    moveToward(PChar, PLeader->loc.p, kFollowDistance, tick);
}

void emote(Bot& bot, PacketSystem& packets, const xi::Emote emote, const CBaseEntity* PTarget = nullptr)
{
    auto  packet       = makePacket<GP_CLI_COMMAND_MOTION>();
    auto* motion       = packet->as<GP_CLI_COMMAND_MOTION>();
    // No target for an emote at nobody: aimed at itself it reads "looks at <name>".
    motion->UniqueNo   = PTarget != nullptr ? PTarget->id : 0;
    motion->ActIndex   = PTarget != nullptr ? PTarget->targid : 0;
    motion->Number     = static_cast<uint8_t>(emote);
    motion->Mode       = static_cast<uint8_t>(xi::EmoteMode::All);
    send(bot, packets, *packet);
}

// A fitting emote back for a player's, or none.
auto emoteReply(const xi::Emote theirs) -> std::optional<xi::Emote>
{
    using E = xi::Emote;
    static const std::map<E, std::vector<E>> replies = {
        { E::Wave, { E::Wave } },                     { E::Bow, { E::Bow } },
        { E::Salute, { E::Salute } },                 { E::Goodbye, { E::Goodbye, E::Wave } },
        { E::Welcome, { E::Bow, E::Smile } },         { E::Cheer, { E::Cheer, E::Hurray } },
        { E::Joy, { E::Joy, E::Cheer } },             { E::Hurray, { E::Hurray, E::Cheer } },
        { E::Clap, { E::Bow, E::Smile } },            { E::Praise, { E::Blush, E::Bow } },
        { E::Smile, { E::Smile } },                   { E::Grin, { E::Grin, E::Laugh } },
        { E::Laugh, { E::Laugh, E::Grin } },          { E::Cry, { E::Comfort } },
        { E::Sulk, { E::Comfort } },                  { E::Sigh, { E::Comfort, E::Sigh } },
        { E::Comfort, { E::Smile, E::Bow } },         { E::Panic, { E::Comfort } },
        { E::Poke, { E::Huh, E::Stare, E::Doubt } },  { E::Slap, { E::Angry, E::Fume, E::Cry } },
        { E::Stare, { E::Stare, E::Huh } },           { E::Blush, { E::Smile } },
        { E::Angry, { E::Panic, E::Doubt, E::Sulk } }, { E::Fume, { E::Comfort, E::Doubt } },
        { E::Point, { E::Huh } },                     { E::Kneel, { E::Bow } },
        { E::Dance, { E::Dance, E::Clap, E::Cheer } }, { E::Doze, { E::Poke } },
        { E::Surprised, { E::Surprised, E::Huh } },   { E::Amazed, { E::Surprised, E::Huh } },
        { E::Shocked, { E::Surprised, E::Huh } },     { E::Doubt, { E::Think, E::Doubt } },
        { E::Think, { E::Think } },                   { E::Huh, { E::Huh, E::Think } },
        { E::Yes, { E::Yes } },                       { E::No, { E::Doubt, E::No } },
        { E::Psych, { E::Huh } },                     { E::Disgusted, { E::Doubt, E::Sigh } },
    };
    const auto it = replies.find(theirs);
    if (it == replies.end())
    {
        return std::nullopt;
    }
    return it->second[xirand::GetRandomNumber<std::size_t>(it->second.size())];
}

void setSitting(Bot& bot, PacketSystem& packets, const bool sitting)
{
    auto packet                             = makePacket<GP_CLI_COMMAND_SIT>();
    packet->as<GP_CLI_COMMAND_SIT>()->Mode  = static_cast<uint32_t>(sitting ? GP_CLI_COMMAND_SIT_MODE::On : GP_CLI_COMMAND_SIT_MODE::Off);
    send(bot, packets, *packet);
    bot.sitting = sitting;
}

// In town a bot strolls from one NPC to another (shops, the auction house, guards) and hangs about there,
// sometimes sitting or using an emote. Elsewhere it roams a little around where it logged in.
constexpr uint16 kEntryEvent = 32000; // a battlefield entrance's menu
constexpr uint8  kEntryTries = 3;
constexpr uint32 kEntryKey   = 0xA0000;

// The leader is already inside: the entry menu gets their battlefield and arena, and the bot comes out of it beside them,
// where a player's client would send the arena's own spot.
auto answerEntry(Bot& bot, PacketSystem& packets, CCharEntity* PChar) -> bool
{
    auto*      PLeader = leaderOf(PChar);
    const auto options = PChar->GetLocalVar("[BCNM]MenuOptions");
    if (PChar->currentEvent->eventId != kEntryEvent || bot.entryAnswered || PLeader == nullptr || PLeader->PBattlefield == nullptr || options == 0)
    {
        return false;
    }

    const auto* PTarget = PChar->currentEvent->targetEntity;
    auto        packet  = makePacket<GP_CLI_COMMAND_EVENTENDXZY>();
    auto*       answer  = packet->as<GP_CLI_COMMAND_EVENTENDXZY>();
    answer->x           = PLeader->loc.p.x;
    answer->y           = PLeader->loc.p.y;
    answer->z           = PLeader->loc.p.z;
    answer->dir         = static_cast<int8_t>(PLeader->loc.p.rotation);
    answer->UniqueNo    = PTarget ? PTarget->id : PChar->id;
    answer->ActIndex    = PTarget ? PTarget->targid : PChar->targid;
    answer->EndPara     = (static_cast<uint32>(std::countr_zero(options)) << 4) | PLeader->PBattlefield->GetArea();
    answer->EventNum    = static_cast<uint16_t>(PChar->getZone());
    answer->EventPara   = kEntryEvent;
    answer->Mode        = 1;
    send(bot, packets, *packet);
    bot.entryAnswered = true;
    return true;
}

// Into the leader's battlefield through the entrance they used, once the game has cleared the bot for it.
// Left outside (not cleared, or it's full) the bot waits where it is until the leader comes back out.
auto followIn(Bot& bot, PacketSystem& packets, CCharEntity* PChar, CCharEntity* PLeader, const timer::time_point tick) -> bool
{
    if (PLeader == nullptr)
    {
        return false;
    }
    if (PLeader->isInEvent() && PLeader->currentEvent->eventId == kEntryEvent && PLeader->currentEvent->targetEntity != nullptr)
    {
        bot.entranceId     = PLeader->currentEvent->targetEntity->id;
        bot.entranceTargid = PLeader->currentEvent->targetEntity->targid;
    }
    if (PLeader->PBattlefield == nullptr || PLeader->getZone() != PChar->getZone())
    {
        bot.entryTries = 0;
        return false;
    }
    if (PChar->PBattlefield != nullptr)
    {
        return false;
    }

    auto* PDoor = bot.entranceId != 0 ? PChar->GetEntity(bot.entranceTargid, TYPE_NPC) : nullptr;
    if (PDoor == nullptr || PDoor->id != bot.entranceId || bot.entryTries >= kEntryTries ||
        !PChar->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Battlefield))
    {
        PChar->PAI->PathFind->Clear();
        return true;
    }
    if (distance(PChar->loc.p, PDoor->loc.p) > 4.0f)
    {
        moveToward(PChar, PDoor->loc.p, 3.0f, tick);
        return true;
    }
    if (throttle(bot, kEntryKey, PDoor->id, std::chrono::seconds(6), tick))
    {
        ++bot.entryTries;
        PChar->PAI->PathFind->Clear();
        sendAction(bot, packets, GP_CLI_COMMAND_ACTION_ACTIONID::Talk, PDoor);
    }
    return true;
}

void wander(Bot& bot, PacketSystem& packets, CCharEntity* PChar, const timer::time_point tick)
{
    auto& path = *PChar->PAI->PathFind;

    if (path.IsFollowingPath())
    {
        path.FollowPath(tick);
        return;
    }

    if (tick < bot.nextWander)
    {
        return;
    }

    const bool town = (static_cast<uint16>(PChar->loc.zone->GetTypeMask()) & static_cast<uint16>(xi::ZoneType::City)) != 0;
    if (!town)
    {
        bot.nextWander = tick + std::chrono::seconds(xirand::GetRandomNumber(20, 90));
        if (path.RoamAround(bot.home, kWanderRadius, 1, 3))
        {
            path.FollowPath(tick);
        }
        return;
    }

    bot.nextWander = tick + std::chrono::seconds(xirand::GetRandomNumber(45, 180));

    // Lingering: sit down or emote now and then before moving on.
    if (const auto roll = xirand::GetRandomNumber<uint32>(100); roll < 35)
    {
        if (roll < 15 && !bot.sitting)
        {
            setSitting(bot, packets, true);
        }
        else if (!bot.sitting)
        {
            static constexpr std::array<xi::Emote, 8> idle = { xi::Emote::Wave, xi::Emote::Think, xi::Emote::Doze, xi::Emote::Laugh,
                                                              xi::Emote::Yes,  xi::Emote::Sigh,  xi::Emote::Cheer, xi::Emote::Stare };
            emote(bot, packets, idle[xirand::GetRandomNumber<std::size_t>(idle.size())]);
        }
        return;
    }

    std::vector<position_t> spots;
    PChar->loc.zone->ForEachNpc(
        [&](CNpcEntity* PNpc)
        {
            if (PNpc->status == xi::Status::Normal && !PNpc->IsNameHidden() && distance(PChar->loc.p, PNpc->loc.p) < 100.0f)
            {
                spots.push_back(PNpc->loc.p);
            }
        });
    if (spots.empty())
    {
        return;
    }

    if (bot.sitting)
    {
        setSitting(bot, packets, false);
    }
    if (path.PathAround(spots[xirand::GetRandomNumber<std::size_t>(spots.size())], 2.5f))
    {
        path.FollowPath(tick);
    }
}

auto partyCanRaise(CCharEntity* PChar) -> bool
{
    return PChar->PParty != nullptr && std::ranges::any_of(PChar->PParty->members,
                                                           [&](CBattleEntity* PMember)
                                                           {
                                                               return PMember != PChar && !PMember->isDead() &&
                                                                      (spell::CanUseSpell(PMember, SpellID::Raise) || spell::CanUseSpell(PMember, SpellID::Raise_II));
                                                           });
}

// A group that is together and healthy enough for the next pull.
auto partyReady(CCharEntity* PLeader) -> bool
{
    return std::ranges::all_of(PLeader->PParty->members,
                               [&](const CBattleEntity* PMember)
                               {
                                   return PMember->getZone() == PLeader->getZone() && !PMember->isDead() && PMember->GetHPP() >= 70 &&
                                          distance(PMember->loc.p, PLeader->loc.p) < 15.0f;
                               });
}

// A bot on its own picks an unclaimed monster near its level and goes for it, like a player soloing.
auto huntTarget(Bot& bot, CCharEntity* PChar, const timer::time_point tick) -> CBattleEntity*
{
    if (tick < bot.nextHunt)
    {
        return nullptr;
    }
    bot.nextHunt = tick + std::chrono::seconds(xirand::GetRandomNumber(8, 25));

    const auto type = static_cast<uint16>(PChar->loc.zone->GetTypeMask());
    if ((type & (static_cast<uint16>(xi::ZoneType::Outdoors) | static_cast<uint16>(xi::ZoneType::Dungeon))) == 0 ||
        PChar->GetHPP() < 80 || (PChar->GetMaxMP() > 0 && PChar->GetMPP() < 50))
    {
        return nullptr;
    }

    const int   level    = PChar->GetMLevel();
    const int   toughest = PChar->PParty != nullptr && PChar->PParty->members.size() >= 3 ? 3 : 1;
    CMobEntity* PBest    = nullptr;
    PChar->loc.zone->ForEachMob(
        [&](CMobEntity* PMob)
        {
            if (PMob->isDead() || !PMob->PAI->IsSpawned() || PMob->PAI->IsEngaged() || PMob->m_OwnerID.isSet() || PMob->m_Type != xi::MobType::Normal)
            {
                return;
            }

            // Easy prey up to an even match.
            const int gap = PMob->GetMLevel() - level;
            if (gap < -6 || gap > toughest || distance(PChar->loc.p, PMob->loc.p) > kHuntRange)
            {
                return;
            }

            if (PBest == nullptr || distance(PChar->loc.p, PMob->loc.p) < distance(PChar->loc.p, PBest->loc.p))
            {
                PBest = PMob;
            }
        });
    return PBest;
}

void fight(Bot& bot, PacketSystem& packets, CCharEntity* PChar, CBattleEntity* PTarget, const timer::time_point tick)
{
    if (!PChar->PAI->IsEngaged())
    {
        // A refused engage (someone else's claim) shouldn't be retried every tick.
        if (throttle(bot, kAttackKey, PTarget->id, std::chrono::seconds(3), tick))
        {
            sendAction(bot, packets, GP_CLI_COMMAND_ACTION_ACTIONID::Attack, PTarget);
        }
    }
    else if (PChar->GetBattleTarget() != PTarget)
    {
        sendAction(bot, packets, GP_CLI_COMMAND_ACTION_ACTIONID::ChangeTarget, PTarget);
    }

    const float reach = PChar->GetMeleeRange(PTarget);
    if (const auto spot = combatSpot(PChar, PTarget, reach * 0.7f))
    {
        moveToSpot(PChar, *spot, tick);
    }
    else
    {
        moveToward(PChar, PTarget->loc.p, reach * 0.8f, tick);
    }

    if (!PChar->PAI->PathFind->IsFollowingPath())
    {
        PChar->PAI->PathFind->LookAt(PTarget->loc.p);
    }
}

// Casters and archers fan out behind the target at range; one that has hate holds its ground and takes it.
void keepRange(CCharEntity* PChar, CBattleEntity* PTarget, const float range, const timer::time_point tick)
{
    if (const auto spot = combatSpot(PChar, PTarget, range))
    {
        moveToSpot(PChar, *spot, tick);
    }
    else
    {
        PChar->PAI->PathFind->Clear();
    }

    if (!PChar->PAI->PathFind->IsFollowingPath())
    {
        PChar->PAI->PathFind->LookAt(PTarget->loc.p);
    }
}

// /heal when low and the leader has stopped, or whenever the leader sits down to /heal; stand up when full or the leader moves on.
// Casters take every spare moment for a tick, missing anything at all; an avatar out would cancel it.
// Melee keep their TP: they rest only when asked, or when low with no TP to lose or nobody to cure them.
auto rest(Bot& bot, PacketSystem& packets, CCharEntity* PChar, CCharEntity* PLeader, const timer::time_point tick) -> bool
{
    const bool resting = PChar->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing);
    const bool full    = PChar->GetHPP() == 100 && (PChar->GetMaxMP() == 0 || PChar->GetMPP() == 100);
    const bool caster  = (isCaster(PChar->GetMJob()) || PChar->GetMJob() == xi::Job::RDM) && PChar->PPet == nullptr;
    const bool below   = PChar->GetHPP() < kRestHPP || (PChar->GetMaxMP() > 0 && PChar->GetMPP() < kRestMPP);
    const bool healer  = PChar->PParty != nullptr && std::ranges::any_of(PChar->PParty->members,
                                                                        [&](const CBattleEntity* PMember)
                                                                        {
                                                                            const auto job = PMember->GetMJob();
                                                                            return PMember != PChar && !PMember->isDead() &&
                                                                                   (job == xi::Job::WHM || job == xi::Job::RDM || job == xi::Job::SCH);
                                                                        });
    const bool low     = bot.restNow || (caster ? !full : below && (PChar->health.tp == 0 || !healer));
    if (full)
    {
        bot.restNow = false;
    }

    bool settled       = true;
    bool leaderResting = false;
    if (PLeader != nullptr)
    {
        leaderResting = PLeader->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing);
        const auto camp = campSpot(PChar);
        settled         = camp ? distance(PChar->loc.p, *camp) <= kCampSpread
                               : PLeader->getZone() == PChar->getZone() && distance(PChar->loc.p, PLeader->loc.p) <= kFollowDistance + 2.0f &&
                          (leaderResting || tick - bot.leaderMovedAt >= kSettleTime);
    }

    // Healing comes before resting: spells can't be cast sitting down.
    const bool heal = healingWanted(PChar);
    if (resting)
    {
        if (full || !settled || heal)
        {
            setResting(bot, packets, false);
            return false;
        }
        return true;
    }

    if ((low || (caster && leaderResting && !full)) && !heal && settled && canAct(PChar))
    {
        PChar->PAI->PathFind->Clear();
        setResting(bot, packets, true);
        return true;
    }
    return false;
}

// A White or Black Mage out of MP sits down mid-fight. It gets up to heal (WHM), once there's MP to nuke again (BLM),
// to sleep an add it has the MP for, or when something starts hitting it.
auto restInFight(Bot& bot, PacketSystem& packets, CCharEntity* PChar, CBattleEntity* PTarget, const timer::time_point tick) -> bool
{
    const auto job = PChar->GetMJob();
    if (job != xi::Job::WHM && job != xi::Job::BLM)
    {
        return false;
    }

    const bool resting  = PChar->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing);
    const bool attacked = PTarget->GetBattleTarget() == PChar || chasedOffTank(PChar);
    const bool wanted   = (job == xi::Job::WHM ? healingWanted(PChar) : PChar->GetMPP() >= kFightResumeMPP) || addNeedsSleep(PChar, PTarget);

    if (resting)
    {
        if (attacked || wanted || PChar->GetMPP() == 100)
        {
            setResting(bot, packets, false);
            return false;
        }
        return true;
    }

    if (PChar->GetMPP() < kFightRestMPP && !attacked && !wanted && canAct(PChar))
    {
        if (throttle(bot, kOutOfMPKey, PTarget->id, std::chrono::minutes(5), tick))
        {
            ShowInfoFmt("bots: {} ({} {}) sits down at {}% MP fighting {} ({}% HP)", PChar->getName(), job == xi::Job::WHM ? "WHM" : "BLM", PChar->GetMLevel(),
                        PChar->GetMPP(), PTarget->getName(), PTarget->GetHPP());
        }
        PChar->PAI->PathFind->Clear();
        setResting(bot, packets, true);
        return true;
    }
    return false;
}

} // namespace

auto Bot::entity() const -> CCharEntity*
{
    return session ? session->PChar.get() : nullptr;
}

void send(Bot& bot, PacketSystem& packets, CBasicPacket& packet)
{
    auto* PChar = bot.entity();
    packets.dispatch(packet.getType(), bot.session.get(), PChar, packet);
    PChar->flushEquipChanges();
}

void castSpell(Bot& bot, PacketSystem& packets, const SpellID spellId, const CBaseEntity* PTarget)
{
    sendAction(bot, packets, GP_CLI_COMMAND_ACTION_ACTIONID::CastMagic, PTarget, static_cast<uint32>(spellId));
}

void useAbility(Bot& bot, PacketSystem& packets, const uint16 abilityId, const CBaseEntity* PTarget)
{
    sendAction(bot, packets, GP_CLI_COMMAND_ACTION_ACTIONID::JobAbility, PTarget, abilityId);
}

// An item from the inventory on a target; false when the bot has none.
auto useItem(Bot& bot, PacketSystem& packets, const uint16 itemId, const CBaseEntity* PTarget) -> bool
{
    auto*      PChar = bot.entity();
    const auto slot  = PChar->getStorage(LOC_INVENTORY)->SearchItem(itemId);
    if (slot == ERROR_SLOTID)
    {
        return false;
    }

    auto  packet            = makePacket<GP_CLI_COMMAND_ITEM_USE>();
    auto* use               = packet->as<GP_CLI_COMMAND_ITEM_USE>();
    use->UniqueNo           = PTarget->id;
    use->ActIndex           = PTarget->targid;
    use->PropertyItemIndex  = slot;
    use->Category           = LOC_INVENTORY;
    send(bot, packets, *packet);
    return true;
}

void rangedAttack(Bot& bot, PacketSystem& packets, const CBaseEntity* PTarget)
{
    sendAction(bot, packets, GP_CLI_COMMAND_ACTION_ACTIONID::Shoot, PTarget);
}

void engage(Bot& bot, PacketSystem& packets, const CBaseEntity* PTarget)
{
    if (throttle(bot, kAttackKey, PTarget->id, std::chrono::seconds(3), timer::now()))
    {
        sendAction(bot, packets, GP_CLI_COMMAND_ACTION_ACTIONID::Attack, PTarget);
    }
}

void disengage(Bot& bot, PacketSystem& packets)
{
    sendAction(bot, packets, GP_CLI_COMMAND_ACTION_ACTIONID::AttackOff, bot.entity());
}

void walkTo(CCharEntity* PChar, const position_t& pos, const float within, const timer::time_point tick)
{
    moveToward(PChar, pos, within, tick);
}

void weaponSkill(Bot& bot, PacketSystem& packets, const uint16 skillId, const CBaseEntity* PTarget)
{
    sendAction(bot, packets, GP_CLI_COMMAND_ACTION_ACTIONID::Weaponskill, PTarget, skillId);
}

// The same request setPos makes; the zone hands the bot over on its next tick.
void travel(CCharEntity* PChar, const xi::ZoneId zone, const position_t& pos)
{
    PChar->loc.p               = pos;
    PChar->loc.destination     = zone;
    PChar->loc.boundary        = 0;
    PChar->m_moghouseID        = 0;
    PChar->status              = xi::Status::Disappear;
    PChar->requestedZoneChange = true;

    if (PChar->shouldPetPersistThroughZoning())
    {
        PChar->setPetZoningInfo();
    }
}

// !bot <name>: what a bot is up to and why, a line each.
auto inspect(const Bot& bot) -> std::vector<std::string>
{
    auto* PChar = bot.entity();
    if (PChar == nullptr || PChar->loc.zone == nullptr)
    {
        return { fmt::format("{} isn't online.", bot.name) };
    }

    static constexpr std::array<const char*, 11> orders = { "cast", "provoke", "attack", "stop", "rest", "stay", "follow", "pull", "stop pulling", "camp", "assist" };

    auto* PLeader = leaderOf(PChar);
    auto* PTarget = PChar->GetBattleTarget();

    std::string doing;
    if (PChar->isDead())
    {
        doing = "dead";
    }
    else if (PChar->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing))
    {
        doing = "resting";
    }
    else if (!PChar->PAI->CanChangeState())
    {
        doing = "casting or using an ability";
    }
    else if (PChar->PAI->IsEngaged() && PTarget != nullptr)
    {
        doing = fmt::format("fighting {}", prettyName(PTarget->getName()));
    }
    else if (PChar->PAI->PathFind->IsFollowingPath())
    {
        doing = "moving";
    }
    else
    {
        doing = "standing";
    }

    std::vector<std::string> lines;
    lines.push_back(fmt::format("{} {}{}: HP {}% MP {}% TP {}, {}, {}", bot.name, static_cast<int>(PChar->GetMJob()), PChar->GetSJob() != xi::Job::NONE ? "+sub" : "",
                                PChar->GetHPP(), PChar->GetMPP(), PChar->health.tp, prettyName(PChar->loc.zone->getName()),
                                PChar->PParty == nullptr ? "no party" : PLeader != nullptr ? fmt::format("following {}", PLeader->getName()) : "leading"));
    const auto named = [](const uint32 id, const char* what)
    {
        const auto* POther = id != 0 ? zoneutils::GetChar(id) : nullptr;
        return POther != nullptr ? fmt::format(", {} {}", what, POther->getName()) : std::string();
    };
    lines.push_back(fmt::format("Now: {}{}{}{}{}", doing, bot.stay ? ", told to stay" : "", named(bot.followId, "following"), named(bot.assistId, "assisting"),
                                bot.withPlayer ? ", in a player's party" : ""));
    lines.push_back(bot.order ? fmt::format("Order: {}{}", orders[static_cast<std::size_t>(bot.order->kind)], bot.queued.empty() ? "" : fmt::format(" (+{} queued)", bot.queued.size()))
                              : "Order: none");
    if (PChar->PParty != nullptr)
    {
        lines.push_back(pullStatus(PChar));
    }
    lines.push_back(hateStatus(PChar, PTarget));
    lines.push_back(fmt::format("Heal wanted: {}; rests below {}% HP / {}% MP{}", healingWanted(PChar) ? "yes" : "no", kRestHPP, kRestMPP,
                                isCaster(PChar->GetMJob()) || PChar->GetMJob() == xi::Job::RDM ? ", and at every chance (caster)" : ""));
    return lines;
}

void leaveParty(Bot& bot, PacketSystem& packets)
{
    if (bot.entity()->PParty != nullptr)
    {
        leaveOrBreakUp(bot, packets, false);
    }
}

void think(Bot& bot, PacketSystem& packets, const std::set<uint32>& botIds, const bool active, const timer::time_point tick)
{
    auto* PChar = bot.entity();

    // Only chat and join requests are read; drop the rest without clearPacketList, which also resets the spawn lists.
    std::optional<std::pair<uint32, uint16>> joinRequest;
    while (!PChar->isPacketListEmpty())
    {
        const auto packet = PChar->popPacket();
        if (packet->getType() == static_cast<uint16>(PacketS2C::GP_SERV_COMMAND_CHAT_STD))
        {
            using Data             = GP_SERV_COMMAND_CHAT_STD::PacketData;
            constexpr auto payload = sizeof(GP_SERV_HEADER);
            const auto*    from    = reinterpret_cast<const char*>(&packet->ref<uint8>(payload + offsetof(Data, sName)));
            const auto*    text    = reinterpret_cast<const char*>(&packet->ref<uint8>(payload + offsetof(Data, Mes)));

            // Auto-translate phrases (0xFD ... 0xFD, with id bytes that can look like letters) and other non-ASCII bytes are dropped.
            std::string message;
            bool        phrase = false;
            for (const char c : std::string_view(text, strnlen(text, sizeof(Data::Mes))))
            {
                if (static_cast<unsigned char>(c) == 0xFD)
                {
                    phrase = !phrase;
                    continue;
                }
                if (!phrase && std::isprint(static_cast<unsigned char>(c)))
                {
                    message += c;
                }
            }

            bot.heard.push_back(Heard{ packet->ref<uint8>(payload + offsetof(Data, Kind)), std::string(from, strnlen(from, sizeof(Data::sName))), std::move(message) });
        }
        else if (packet->getType() == static_cast<uint16>(PacketS2C::GP_SERV_COMMAND_MOTIONMES))
        {
            using Data             = GP_SERV_COMMAND_MOTIONMES::PacketData;
            constexpr auto payload = sizeof(GP_SERV_HEADER);
            const auto     caster  = packet->ref<uint32>(payload + offsetof(Data, CasUniqueNo));
            const auto     target  = packet->ref<uint32>(payload + offsetof(Data, TarUniqueNo));
            const auto     theirs  = static_cast<xi::Emote>(packet->ref<uint16>(payload + offsetof(Data, MesNum)));
            auto*          PCaster = caster != PChar->id && !isPoolChar(caster) ? zoneutils::GetChar(caster) : nullptr;

            if (PCaster != nullptr)
            {
                ShowInfoFmt("bots: emote from {}: target {} index {}, caster index {}, emote {}, mode {}, param {}", PCaster->getName(), target,
                            packet->ref<uint16>(payload + offsetof(Data, TarActIndex)), packet->ref<uint16>(payload + offsetof(Data, CasActIndex)),
                            static_cast<uint16>(theirs), static_cast<uint8>(packet->ref<uint8>(payload + offsetof(Data, Mode))),
                            packet->ref<uint16>(payload + offsetof(Data, Param)));
            }

            const bool atMe   = target == PChar->id;
            const bool nearby = (target == 0 || target == caster) && PCaster != nullptr && distance(PCaster->loc.p, PChar->loc.p) <= kEmoteNear &&
                                xirand::GetRandomNumber<uint32>(100) < kEmoteChance;
            if (PCaster != nullptr && (atMe || nearby))
            {
                if (const auto reply = emoteReply(theirs))
                {
                    bot.emoteBack = static_cast<uint8>(*reply);
                    bot.emoteTo   = caster;
                    bot.emoteAt   = tick + std::chrono::milliseconds(xirand::GetRandomNumber(800, 2500));
                }
            }
        }
        else if (packet->getType() == static_cast<uint16>(PacketS2C::GP_SERV_COMMAND_PARTYREQ))
        {
            using Data             = GP_SERV_COMMAND_PARTYREQ::PacketData;
            constexpr auto payload = sizeof(GP_SERV_HEADER);
            if (packet->ref<uint8>(payload + offsetof(Data, Status)) == static_cast<uint8>(GP_SERV_COMMAND_PARTYREQ_STATUS::Requesting))
            {
                joinRequest = { packet->ref<uint32>(payload + offsetof(Data, UniqueNo)), packet->ref<uint16>(payload + offsetof(Data, ActIndex)) };
            }
        }
    }

    if (PChar->loc.zone == nullptr || PChar->status == xi::Status::Disappear)
    {
        return;
    }

    if (PChar->isDead())
    {
        if (bot.diedAt == timer::time_point{})
        {
            bot.diedAt = tick;
        }

        // Take a raise when one is offered; wait longer for one when someone in the party, or close by, could cast it.
        if (PChar->m_hasRaise)
        {
            sendAction(bot, packets, GP_CLI_COMMAND_ACTION_ACTIONID::RaiseMenu, PChar);
        }
        else if (tick - bot.diedAt > (partyCanRaise(PChar) || raiserNearby(PChar) ? kRaiseWait : kHomepointDelay))
        {
            bot.diedAt = {};
            sendAction(bot, packets, GP_CLI_COMMAND_ACTION_ACTIONID::HomepointMenu, PChar);
        }
        return;
    }
    bot.diedAt = {};

    if (PChar->isInEvent())
    {
        if (!answerEntry(bot, packets, PChar))
        {
            finishEvent(bot, packets, PChar);
        }
        return;
    }
    bot.entryAnswered = false;

    // Bots may party among themselves, but one a real player has left breaks up.
    // A zoning player drops out of the party's member list for a moment, so only a real absence counts.
    if (PChar->PParty == nullptr)
    {
        bot.withPlayer   = false;
        bot.playerGoneAt = {};
    }
    else if (hasRealPlayer(PChar->PParty, botIds))
    {
        if (!bot.withPlayer)
        {
            greet(bot);
        }
        bot.withPlayer   = true;
        bot.playerGoneAt = {};
    }
    else if (bot.withPlayer)
    {
        if (bot.playerGoneAt == timer::time_point{})
        {
            bot.playerGoneAt = tick;
        }
        else if (tick - bot.playerGoneAt > kPlayerGoneGrace)
        {
            bot.withPlayer   = false;
            bot.playerGoneAt = {};
            leaveOrBreakUp(bot, packets, true);
            return;
        }
    }

    if (PChar->InvitePending.entity.UniqueNo != 0 && PChar->PParty == nullptr)
    {
        acceptInvite(bot, packets);
    }
    else if (joinRequest && (PChar->PParty == nullptr || PChar->PParty->GetLeader() == PChar))
    {
        invite(bot, packets, joinRequest->first, joinRequest->second);
    }
    else if (!bot.invites.empty() && (PChar->PParty == nullptr || (PChar->PParty->GetLeader() == PChar && !PChar->PParty->IsFull())))
    {
        const auto name = bot.invites.front();
        bot.invites.erase(bot.invites.begin());
        if (auto* PTarget = zoneutils::GetCharByName(name); PTarget != nullptr && PTarget->PParty == nullptr && PTarget != PChar)
        {
            invite(bot, packets, PTarget->id, PTarget->getZone() == PChar->getZone() ? PTarget->targid : 0);
        }
    }
    else if (PChar->PParty != nullptr && PChar->PParty->GetLeader() != PChar)
    {
        bot.invites.clear();
    }

    if (bot.seeking != (PChar->PParty == nullptr))
    {
        setSeeking(bot, packets, PChar->PParty == nullptr);
    }


    auto* PLeader = leaderOf(PChar);
    auto* PGuide  = partner(PChar, bot.followId, PLeader);
    auto* PTarget = combatTarget(bot, PChar, partner(PChar, bot.assistId, PLeader));

    if (bot.sitting && (PTarget != nullptr || PLeader != nullptr))
    {
        setSitting(bot, packets, false);
    }

    // No real player around to see it: the bot stands idle, only fighting back if something attacks it.
    if (!active && (PTarget == nullptr || PTarget->GetBattleTarget() != PChar))
    {
        PChar->PAI->PathFind->Clear();
        return;
    }

    if (PGuide != nullptr)
    {
        if (distance(PGuide->loc.p, bot.leaderLast) >= 0.1f)
        {
            bot.leaderMovedAt = tick;
        }
        bot.leaderLast = PGuide->loc.p;
    }

    // Mid-cast (or ability, weapon skill, item): a player stands still and waits, or the cast is interrupted.
    if (!PChar->PAI->CanChangeState())
    {
        PChar->PAI->PathFind->Clear();
        return;
    }

    if (bot.emoteTo != 0 && tick >= bot.emoteAt)
    {
        if (auto* PTo = zoneutils::GetChar(bot.emoteTo); PTo != nullptr && PTo->getZone() == PChar->getZone() && distance(PTo->loc.p, PChar->loc.p) < 30.0f)
        {
            PChar->PAI->PathFind->LookAt(PTo->loc.p);
            emote(bot, packets, static_cast<xi::Emote>(bot.emoteBack), PTo);
        }
        bot.emoteTo = 0;
    }

    // Something attacking a bot left outside a battlefield still gets fought.
    if (followIn(bot, packets, PChar, PLeader, tick) && PTarget == nullptr)
    {
        return;
    }

    if (followOrder(bot, packets, tick))
    {
        PChar->PAI->PathFind->Clear();
        return;
    }

    if (pull(bot, packets, PChar, PLeader, tick))
    {
        return;
    }

    if (PTarget != nullptr)
    {
        bot.foughtAt = tick;

        if (restInFight(bot, packets, PChar, PTarget, tick))
        {
            return;
        }

        // Camped: a monster still on its way in is waited for, not met. The tank opens on it from here.
        if (const auto camp = campSpot(PChar); camp && distance(PTarget->loc.p, *camp) > kCampHold && PTarget->GetBattleTarget() != nullptr &&
                                               PTarget->GetBattleTarget()->GetHPP() >= kHelpBelowHPP)
        {
            if (PChar->PAI->IsEngaged())
            {
                sendAction(bot, packets, GP_CLI_COMMAND_ACTION_ACTIONID::AttackOff, PChar);
            }

            // On the one it's after: back off across camp, still inside it. A melee monster follows all the way in;
            // an archer or caster stops at its range from here, which puts it inside camp too.
            if (PTarget->GetBattleTarget() == PChar)
            {
                const float dx  = camp->x - PTarget->loc.p.x;
                const float dz  = camp->z - PTarget->loc.p.z;
                const float len = std::max(0.01f, std::sqrt(dx * dx + dz * dz));

                position_t away = *camp;
                away.x += dx / len * (kCampHold - 1.0f);
                away.z += dz / len * (kCampHold - 1.0f);
                if (distance(PChar->loc.p, away) > 1.5f)
                {
                    moveToward(PChar, away, 1.0f, tick);
                    return;
                }
                PChar->PAI->PathFind->Clear();
                PChar->PAI->PathFind->LookAt(PTarget->loc.p);
                act(bot, packets, PTarget, false, tick);
                return;
            }

            if (distance(PChar->loc.p, *camp) > kCampSpread)
            {
                moveToSpot(PChar, spotNear(PChar->getZone(), *camp, 1.5f, kCampSpread - 1.0f), tick);
                return;
            }
            PChar->PAI->PathFind->Clear();
            PChar->PAI->PathFind->LookAt(PTarget->loc.p);
            act(bot, packets, PTarget, false, tick);
            return;
        }

        if (PChar->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing))
        {
            setResting(bot, packets, false);
        }

        // Casters and archers hit by a link run to the tank too; melee keep swinging while the tank grabs it.
        if (pulledHate(PChar, PTarget) || ((isCaster(PChar->GetMJob()) || isRanged(PChar)) && chasedOffTank(PChar)))
        {
            // Hate came off the tank: stop adding to it and bring the monster back to them.
            if (PChar->PAI->IsEngaged())
            {
                sendAction(bot, packets, GP_CLI_COMMAND_ACTION_ACTIONID::AttackOff, PChar);
            }
            if (auto* PTank = mainTank(PChar); distance(PChar->loc.p, PTank->loc.p) > kToTankDistance)
            {
                moveToward(PChar, PTank->loc.p, kToTankDistance / 2, tick);
            }
            else if (!PChar->PAI->PathFind->IsFollowingPath())
            {
                PChar->PAI->PathFind->LookAt(PTarget->loc.p);
            }
        }
        else if (isCaster(PChar->GetMJob()) || isRanged(PChar))
        {
            if (PChar->PAI->IsEngaged())
            {
                sendAction(bot, packets, GP_CLI_COMMAND_ACTION_ACTIONID::AttackOff, PChar);
            }
            keepRange(PChar, PTarget, isRanged(PChar) ? kArcherDistance : kCasterDistance, tick);
        }
        else
        {
            fight(bot, packets, PChar, PTarget, tick);
        }

        act(bot, packets, PTarget, false, tick);
        return;
    }

    if (PChar->PAI->IsEngaged())
    {
        sendAction(bot, packets, GP_CLI_COMMAND_ACTION_ACTIONID::AttackOff, PChar);
    }

    // Following a leader who is moving, or still catching up: no casting that would stop the bot.
    const bool travelling = PGuide != nullptr && !campSpot(PChar) && (tick - bot.leaderMovedAt < kSettleTime ||
                                                  distance(PChar->loc.p, PGuide->loc.p) > kFollowDistance + 3.0f || PGuide->getZone() != PChar->getZone());

    if (act(bot, packets, nullptr, travelling, tick) || (PGuide != nullptr && conceal(bot, packets, PGuide, tick)) || rest(bot, packets, PChar, PLeader, tick))
    {
        return;
    }

    if (PGuide != nullptr)
    {
        if (const auto camp = campSpot(PChar))
        {
            if (distance(PChar->loc.p, *camp) > kCampSpread)
            {
                moveToSpot(PChar, spotNear(PChar->getZone(), *camp, 1.5f, kCampSpread - 1.0f), tick);
            }
        }
        else if (!bot.stay)
        {
            follow(PChar, PGuide, tick);
        }
    }
    else if (PChar->PParty != nullptr && PChar->PParty->GetLeader() == PChar && (campSpot(PChar) || partyReady(PChar)) && autoCamp(PChar, bot.withPlayer))
    {
        // A bot-led party at its camp: the leader stays there and the puller brings the fights in.
        if (const auto camp = campSpot(PChar); camp && distance(PChar->loc.p, *camp) > kCampSpread)
        {
            moveToSpot(PChar, spotNear(PChar->getZone(), *camp, 1.5f, kCampSpread - 1.0f), tick);
        }
    }
    else if (PChar->PParty == nullptr || (PChar->PParty->GetLeader() == PChar && partyReady(PChar)))
    {
        // A body nearby: go over and raise it before anything else.
        if (auto* PDead = deadToRaise(PChar); PDead != nullptr && distance(PChar->loc.p, PDead->loc.p) > kRaiseRange)
        {
            moveToward(PChar, PDead->loc.p, kRaiseRange - 3.0f, tick);
            return;
        }

        if (auto* PPrey = settings::get<bool>("bots.HUNT") ? huntTarget(bot, PChar, tick) : nullptr)
        {
            bot.focusTarget = PPrey->id;
            return;
        }
        wander(bot, packets, PChar, tick);
    }
}

} // namespace bots
