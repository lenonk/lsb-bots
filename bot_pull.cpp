//
// Pulling: one bot, asked in chat, brings monsters back to camp.
//
//   Camp is where the leader stood when the puller was asked (or said "camp here" last). While the
//   leader is away from it, pulling pauses and the puller follows like everyone else.
//
//   Once the monster at camp is nearly dead the puller leaves it, picks the next target and waits
//   within reach of it. It pulls as soon as nobody would sit down to rest (the same thresholds as
//   rest(), so it never waits on someone who isn't low enough to rest), even with that last monster
//   still up, but never into a fight with more than one monster or a hurt tank. The party leaves the
//   monster alone until the puller brings it in, or until the puller is in trouble.
//

#include "bots.h"

#include "common/logging.h"

#include "map/ai/ai_container.h"
#include "map/ai/helpers/pathfind.h"
#include "map/entities/char_entity.h"
#include "map/entities/mob_entity.h"
#include "map/party.h"
#include "map/status_effect.h"
#include "map/status_effect_container.h"
#include "map/utils/zoneutils.h"
#include "map/zone.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#include <map>
#include <optional>

namespace bots
{

namespace
{

enum class Stage : uint8
{
    Waiting,   // fighting at camp until the monster there is nearly dead
    Scouting,  // out by a chosen monster, waiting for the party to recover
    Returning, // bringing it back
};

struct Pull
{
    uint32            pullerId{};
    xi::ZoneId        zone{};
    position_t        camp{};
    Stage             stage{};
    EntityId          mob;
    timer::time_point stageAt{};
    timer::time_point deadline{}; // give up on this stage after
    bool              announced{}; // "Pulling X" goes out once per pull
};

// By party leader.
std::map<uint32, Pull> pulls;

constexpr float kLinkRadius   = 8.0f;  // other monsters this close to a target might link
constexpr int   kWeakestGap   = -3;    // levels below the party's highest, for the usual pick
constexpr int   kFallbackGap  = -9;    // and when nothing else is around
constexpr float kPullLimit    = 1000.0f; // furthest from camp a puller goes
constexpr float kCampReached  = 3.0f;
constexpr float kWaitBeyond   = 6.0f;  // scouting stands this much further out than its pull range
constexpr auto  kSpareTime    = std::chrono::seconds(45); // on top of the running time, for a stage's time limit
constexpr float kCampDrift    = 20.0f; // the leader this far from camp pauses pulling
constexpr uint8 kTroubleHPP   = 40;
constexpr uint8 kLeaveAtHPP   = 10; // the puller leaves the monster at camp to the rest at this
constexpr uint8 kTankPullHPP  = 75; // the main tank's HP before another comes in on top of one
constexpr float kRunSpeed     = 5.0f; // yalms a second
constexpr float kClaimSeconds = 5.0f;
constexpr float kTankGrab     = 15.0f; // the main tank takes the pulled monster once it's this close
constexpr float kPullerHome   = 8.0f;  // the puller this close to camp is back, and everyone joins in

constexpr float kRestTick = 10.0f; // seconds between /heal ticks

auto leaderOfParty(const CCharEntity* PChar) -> CBattleEntity*
{
    return PChar->PParty != nullptr ? PChar->PParty->GetLeader() : nullptr;
}

auto pullFor(const CCharEntity* PChar) -> Pull*
{
    auto* PLeader = leaderOfParty(PChar);
    if (PLeader == nullptr)
    {
        return nullptr;
    }
    const auto it = pulls.find(PLeader->id);
    return it != pulls.end() ? &it->second : nullptr;
}

auto inPartyOf(const CCharEntity* PChar, const CBattleEntity* PEntity) -> bool
{
    return PEntity != nullptr && PChar->PParty != nullptr && std::ranges::contains(PChar->PParty->members, PEntity);
}

// Monsters on the party right now, leaving out the one being pulled.
auto partyMobs(CCharEntity* PChar) -> std::vector<CMobEntity*>
{
    std::vector<CMobEntity*> mobs;
    PChar->loc.zone->ForEachMob(
        [&](CMobEntity* PMob)
        {
            if (PMob->isAlive() && PMob->PAI->IsEngaged() && inPartyOf(PChar, PMob->GetBattleTarget()) && !beingPulled(PChar, PMob))
            {
                mobs.push_back(PMob);
            }
        });
    return mobs;
}

// HP and MP regained resting over the next seconds, as scripts/effects/healing.lua gives them: a tick every 10 s with
// nothing on the first, then 10 HP and 12 MP, one more each tick (more MP with Clear Mind). Someone not resting yet is
// counted from sitting down now. Refresh adds its own.
auto restGain(CBattleEntity* PMember, const float seconds) -> std::pair<int32, int32>
{
    int   tickNo = 1;                // what the next tick's count will be
    float wait   = kRestTick + 1.0f; // seconds until it comes, a second to sit down first
    if (const auto* PHeal = PMember->StatusEffectContainer->GetStatusEffect(xi::StatusEffect::Healing))
    {
        tickNo            = PHeal->GetElapsedTickCount() + 1;
        const float since = std::chrono::duration<float>(timer::now() - PHeal->GetStartTime()).count();
        wait              = std::max(0.0f, static_cast<float>(tickNo) * kRestTick - since);
    }

    int32     hp    = 0;
    int32     mp    = 0;
    const int clear = 1 + PMember->getMod(xi::Mod::CLEAR_MIND);
    for (; wait <= seconds; ++tickNo, wait += kRestTick)
    {
        if (tickNo >= 2)
        {
            hp += 10 + (tickNo - 2) + PMember->getMod(xi::Mod::HPHEAL);
            mp += 12 + (tickNo - 2) * clear + PMember->getMod(xi::Mod::MPHEAL);
        }
    }

    if (const auto* PRefresh = PMember->StatusEffectContainer->GetStatusEffect(xi::StatusEffect::Refresh);
        PRefresh != nullptr && PRefresh->GetTickTime() > timer::duration::zero())
    {
        mp += PRefresh->GetPower() * static_cast<int32>(seconds / std::chrono::duration<float>(PRefresh->GetTickTime()).count());
    }
    return { hp, mp };
}

// By the time the puller is back, nobody would need to sit down: anyone resting is counted at what they'll have
// regained, and Weakness only counts if it will still be on. The party's healer needs more MP than the rest.
auto readyBy(CCharEntity* PChar, const float seconds) -> bool
{
    auto* PHealer = partyHealer(PChar);
    const auto arrival = timer::now() + std::chrono::milliseconds(static_cast<int64>(seconds * 1000));
    for (auto* PMember : PChar->PParty->members)
    {
        if (PMember->isDead() || PMember->getZone() != PChar->getZone())
        {
            return false;
        }

        if (const auto* PWeak = PMember->StatusEffectContainer->GetStatusEffect(xi::StatusEffect::Weakness);
            PWeak != nullptr && PWeak->GetStartTime() + PWeak->GetDuration() > arrival)
        {
            return false;
        }

        const bool hasMP   = PMember->GetMaxMP() > 0;
        // Casters without a pet out sit down at every chance; melee only recover by being cured, unless they're actually resting.
        const auto job     = PMember->GetMJob();
        const bool resting = PMember->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing) ||
                             ((isCaster(job) || job == xi::Job::RDM) && PMember->PPet == nullptr);
        const auto [hpGain, mpGain] = resting ? restGain(PMember, seconds) : std::pair<int32, int32>{};
        const float hp     = 100.0f * static_cast<float>(PMember->health.hp + hpGain) / static_cast<float>(std::max(1, PMember->GetMaxHP()));
        const float mp     = hasMP ? 100.0f * static_cast<float>(PMember->health.mp + mpGain) / static_cast<float>(PMember->GetMaxMP()) : 100.0f;
        if (hp < kRestHPP || (hasMP && mp < (PMember == PHealer ? kHealerPullMPP : kRestMPP)))
        {
            return false;
        }
    }
    return true;
}

// Nothing up, or one monster nearly dead: time to go find the next.
auto windingDown(CCharEntity* PChar) -> bool
{
    const auto mobs = partyMobs(PChar);
    return mobs.empty() || (mobs.size() == 1 && mobs.front()->GetHPP() <= kLeaveAtHPP);
}

// Another can come in: everyone will be fit when it arrives, and on top of at most one monster with the tank still healthy.
auto safeToPull(CCharEntity* PChar, const float tripSeconds) -> bool
{
    const auto mobs = partyMobs(PChar);
    if (!readyBy(PChar, tripSeconds) || mobs.size() > 1)
    {
        return false;
    }
    auto* PTank = mainTank(PChar);
    return mobs.empty() || PTank == nullptr || PTank->GetHPP() >= kTankPullHPP;
}

// More than one monster on the party, or someone going down: come back and help.
auto danger(CCharEntity* PChar) -> bool
{
    return partyMobs(PChar).size() > 1 ||
           std::ranges::any_of(PChar->PParty->members, [](const CBattleEntity* PMember) { return !PMember->isDead() && PMember->GetHPP() < kTroubleHPP; });
}

auto levelOf(const CCharEntity* PChar) -> int
{
    int level = 0;
    for (const auto* PMember : PChar->PParty->members)
    {
        level = std::max<int>(level, PMember->GetMLevel());
    }
    return level;
}

// Why a monster can't be pulled, or nullptr when it can.
auto unpullable(CMobEntity* PMob, const position_t& camp, const int level, const int weakest, const int toughest) -> const char*
{
    const int gap = PMob->GetMLevel() - level;
    if (!PMob->isAlive() || !PMob->PAI->IsSpawned())
    {
        return "not up";
    }
    if (PMob->PAI->IsEngaged() || PMob->m_OwnerID.isSet())
    {
        return "busy or claimed";
    }
    if (PMob->m_Type != xi::MobType::Normal)
    {
        return "special";
    }
    if (gap < weakest || gap > toughest)
    {
        return "wrong level";
    }
    if (distance(camp, PMob->loc.p) > kPullLimit)
    {
        return "too far";
    }
    return nullptr;
}

// Exp parties pull above themselves: a full party up to very tough, a small one not much past an even match.
auto toughestGap(const CCharEntity* PChar) -> int
{
    const auto size = PChar->PParty != nullptr ? PChar->PParty->members.size() : 1;
    return size >= 5 ? 5 : size >= 3 ? 3 : 2;
}

auto pullable(const CCharEntity* PChar, CMobEntity* PMob, const position_t& camp, const int level, const int weakest = kWeakestGap) -> bool
{
    return unpullable(PMob, camp, level, weakest, toughestGap(PChar)) == nullptr;
}

// The nearest unclaimed monster to camp anywhere in the zone near the party's level, one standing alone (less likely to
// link) over one in a group; weaker ones only when there's nothing in the usual level range.
auto pickTarget(CCharEntity* PChar, const position_t& camp) -> CMobEntity*
{
    const int level = levelOf(PChar);
    for (const int weakest : { kWeakestGap, kFallbackGap })
    {
        CMobEntity* PBest        = nullptr;
        bool        bestCrowded  = true;
        PChar->loc.zone->ForEachMob(
            [&](CMobEntity* PMob)
            {
                if (!pullable(PChar, PMob, camp, level, weakest))
                {
                    return;
                }

                bool crowded = false;
                PChar->loc.zone->ForEachMob(
                    [&](CMobEntity* POther)
                    {
                        crowded = crowded || (POther != PMob && POther->isAlive() && POther->PAI->IsSpawned() && distance(POther->loc.p, PMob->loc.p) < kLinkRadius);
                    });

                if (PBest == nullptr || (bestCrowded && !crowded) ||
                    (crowded == bestCrowded && distance(camp, PMob->loc.p) < distance(camp, PBest->loc.p)))
                {
                    PBest       = PMob;
                    bestCrowded = crowded;
                }
            });
        if (PBest != nullptr)
        {
            return PBest;
        }
    }

    // Nothing: say why, now and then, counting the monsters within reach of camp by reason.
    static timer::time_point lastReport{};
    if (timer::now() - lastReport > std::chrono::seconds(30))
    {
        lastReport = timer::now();
        std::map<std::string, int> reasons;
        PChar->loc.zone->ForEachMob(
            [&](CMobEntity* PMob)
            {
                const auto* why = unpullable(PMob, camp, level, kFallbackGap, toughestGap(PChar));
                ++reasons[why != nullptr ? why : "ok"];
            });
        std::string summary;
        for (const auto& [why, count] : reasons)
        {
            summary += fmt::format(" {}={}", why, count);
        }
        ShowInfoFmt("bots: {} found nothing to pull (party level {}):{}", PChar->getName(), level, summary.empty() ? " no monsters in the zone" : summary);
    }
    return nullptr;
}

auto runTime(const float yalms) -> std::chrono::milliseconds
{
    return std::chrono::milliseconds(static_cast<int64>(yalms / kRunSpeed * 1000.0f));
}

// A spot on the camp side of the target, a little beyond pull range.
auto waitSpot(const position_t& mob, const position_t& camp, const float range) -> position_t
{
    const float dx   = camp.x - mob.x;
    const float dz   = camp.z - mob.z;
    const float len  = std::max(0.01f, std::sqrt(dx * dx + dz * dz));
    const float away = std::min(len, range + kWaitBeyond);

    position_t spot = mob;
    spot.x += dx / len * away;
    spot.z += dz / len * away;
    return spot;
}

void finish(Pull& pull, const timer::time_point tick, const char* why)
{
    ShowInfoFmt("bots: pull by {} ended: {}", pull.pullerId, why);
    pull.announced = false;
    pull.stage   = Stage::Waiting;
    pull.mob     = {};
    pull.stageAt = tick;
}

} // namespace

// For !bot: the party's camp and puller, and where the pull is at.
auto pullStatus(const CCharEntity* PChar) -> std::string
{
    const auto* pull    = pullFor(PChar);
    const auto* PLeader = leaderOfParty(PChar);
    if (pull == nullptr || PLeader == nullptr)
    {
        return "Camp: none";
    }

    static constexpr std::array<const char*, 3> stages = { "waiting", "out scouting", "bringing it back" };
    std::string puller = "nobody pulling";
    if (pull->pullerId != 0)
    {
        const auto* PPuller = zoneutils::GetChar(pull->pullerId);
        const auto* PMob    = pull->mob.resolve<CMobEntity>();
        puller = fmt::format("puller {} {}{}", PPuller ? PPuller->getName() : std::string("?"), stages[static_cast<std::size_t>(pull->stage)],
                             PMob ? fmt::format(" ({} lv{})", prettyName(PMob->getName()), PMob->GetMLevel()) : std::string());
    }
    const bool here = campSpot(PChar).has_value();
    return fmt::format("Camp: {:.0f},{:.0f}, leader {:.0f}y from it{}; {}", pull->camp.x, pull->camp.z, distance(PLeader->loc.p, pull->camp),
                       here ? "" : " (paused)", puller);
}

void setPuller(CBattleEntity* PLeader, const uint32 pullerId)
{
    pulls[PLeader->id] = Pull{ .pullerId = pullerId, .zone = PLeader->getZone(), .camp = PLeader->loc.p };
    ShowInfoFmt("bots: {} is pulling for {}'s party", pullerId, PLeader->getName());
}

void stopPulling(CBattleEntity* PLeader)
{
    if (const auto it = pulls.find(PLeader->id); it != pulls.end())
    {
        it->second.pullerId = 0;
        it->second.stage    = Stage::Waiting;
    }
}

void setCamp(CBattleEntity* PLeader)
{
    auto& pull = pulls[PLeader->id];
    pull.zone  = PLeader->getZone();
    pull.camp  = PLeader->loc.p;
    ShowInfoFmt("bots: {}'s party camps at {:.0f},{:.0f}", PLeader->getName(), pull.camp.x, pull.camp.z);
}

void breakCamp(CBattleEntity* PLeader)
{
    pulls.erase(PLeader->id);
}

// A party of three or more bots on their own camps where its leader stands, with its best puller pulling.
// The caller asks once the party is together and fit; true while the party is camped.
auto autoCamp(CCharEntity* PLeader, const bool withPlayer) -> bool
{
    if (pullFor(PLeader) != nullptr)
    {
        return campSpot(PLeader).has_value();
    }

    const auto type    = static_cast<uint16>(PLeader->loc.zone->GetTypeMask());
    const bool hunting = (type & (static_cast<uint16>(xi::ZoneType::Outdoors) | static_cast<uint16>(xi::ZoneType::Dungeon))) != 0 &&
                         (type & static_cast<uint16>(xi::ZoneType::City)) == 0;
    if (withPlayer || !hunting || PLeader->PParty == nullptr || PLeader->PParty->members.size() < 3)
    {
        return false;
    }

    CBattleEntity* PPuller = nullptr;
    for (auto* PMember : PLeader->PParty->members)
    {
        if (PMember != PLeader && isPoolChar(PMember->id) &&
            (PPuller == nullptr || pullerRank(PMember->GetMJob(), PMember->GetMLevel()) < pullerRank(PPuller->GetMJob(), PPuller->GetMLevel())))
        {
            PPuller = PMember;
        }
    }
    if (PPuller == nullptr)
    {
        return false;
    }

    setPuller(PLeader, PPuller->id);
    return true;
}

// Where this bot's party is camped, while the leader is still there; not in towns.
auto campSpot(const CCharEntity* PChar) -> std::optional<position_t>
{
    const auto* pull    = pullFor(PChar);
    const auto* PLeader = leaderOfParty(PChar);
    if (pull == nullptr || PLeader == nullptr || pull->zone != PChar->getZone() || PLeader->getZone() != PChar->getZone() ||
        distance(PLeader->loc.p, pull->camp) > kCampDrift || PChar->loc.zone == nullptr ||
        (static_cast<uint16>(PChar->loc.zone->GetTypeMask()) & static_cast<uint16>(xi::ZoneType::City)) != 0)
    {
        return std::nullopt;
    }
    return pull->camp;
}

// A monster on the puller, while the puller is fit and still out from camp, is left to the puller: everyone else waits at
// camp, except the main tank, which takes it once it's in reach.
auto beingPulled(const CCharEntity* PChar, const CBattleEntity* PMob) -> bool
{
    const auto* pull = pullFor(PChar);
    if (pull == nullptr || pull->pullerId == 0 || PChar->id == pull->pullerId)
    {
        return false;
    }

    const auto* PVictim = PMob->GetBattleTarget();
    if (PVictim == nullptr || PVictim->id != pull->pullerId || PVictim->GetHPP() < kTroubleHPP || distance(PVictim->loc.p, pull->camp) <= kPullerHome)
    {
        return false;
    }

    return !(mainTank(const_cast<CCharEntity*>(PChar)) == PChar && distance(PChar->loc.p, PMob->loc.p) <= kTankGrab);
}

auto pull(Bot& bot, PacketSystem& packets, CCharEntity* PChar, CCharEntity* PLeader, const timer::time_point tick) -> bool
{
    auto* pull = pullFor(PChar);
    if (pull == nullptr || pull->pullerId != PChar->id || PLeader == nullptr || PLeader->getZone() != PChar->getZone())
    {
        return false;
    }

    // The leader has wandered off from camp (or it's a town): pause, and finish bringing back anything already pulled.
    const auto camp = pull->camp;
    const bool town = (static_cast<uint16>(PChar->loc.zone->GetTypeMask()) & static_cast<uint16>(xi::ZoneType::City)) != 0;
    if (town || pull->zone != PChar->getZone() || distance(PLeader->loc.p, camp) > kCampDrift)
    {
        if (pull->stage == Stage::Scouting)
        {
            finish(*pull, tick, "leader left camp");
        }
        if (pull->stage == Stage::Waiting)
        {
            return false;
        }
    }

    auto* PMob = pull->mob.resolve<CMobEntity>();

    switch (pull->stage)
    {
        case Stage::Waiting:
        {
            if (!windingDown(PChar))
            {
                return false;
            }
            PMob = pickTarget(PChar, camp);
            if (PMob == nullptr)
            {
                return false;
            }
            pull->mob      = EntityId(PMob);
            pull->stage    = Stage::Scouting;
            pull->stageAt  = tick;
            pull->deadline = tick + runTime(distance(PChar->loc.p, PMob->loc.p)) * 2 + kSpareTime;
            ShowInfoFmt("bots: {} is going for {} (level {})", PChar->getName(), PMob->getName(), PMob->GetMLevel());
            [[fallthrough]];
        }
        case Stage::Scouting:
        {
            // Ours once it's on the puller or claimed by anyone in the party.
            const bool ours = PMob != nullptr && (PMob->GetBattleTarget() == PChar || (PMob->m_OwnerID.isSet() && std::ranges::any_of(PChar->PParty->members,
                                                                                                                                    [&](const CBattleEntity* PMember)
                                                                                                                                    { return PMember->id == PMob->m_OwnerID.UniqueNo; })));

            // Trouble at camp, or the target wandered off, got claimed by someone else, or is taking too long.
            if (PMob == nullptr || (!ours && (tick > pull->deadline || danger(PChar) || !pullable(PChar, PMob, camp, levelOf(PChar), kFallbackGap))))
            {
                finish(*pull, tick, PMob == nullptr ? "target gone" : tick > pull->deadline ? "took too long" : danger(PChar) ? "trouble at camp" : "target taken or moved off");
                return false;
            }

            // Leaving the last one at camp to the others.
            if (PChar->PAI->IsEngaged() && PChar->GetBattleTarget() != PMob)
            {
                disengage(bot, packets);
            }

            // Claimed, or it noticed the puller first: that's the pull.
            if (ours)
            {
                pull->stage    = Stage::Returning;
                pull->stageAt  = tick;
                pull->deadline = tick + runTime(distance(PChar->loc.p, camp)) * 2 + kSpareTime;
                const auto* PHealer = partyHealer(PChar);
                ShowInfoFmt("bots: {} pulled {}{}", PChar->getName(), PMob->getName(),
                            PHealer != nullptr ? fmt::format(" ({} at {}% MP)", PHealer->getName(), PHealer->GetMPP()) : std::string());
                if (!pull->announced)
                {
                    pull->announced = true;
                    callout(bot, fmt::format("Pulling {} - incoming!", prettyName(PMob->getName())), false);
                }
                return true;
            }

            // The round trip: out to it, a few seconds to claim, and back.
            const float range = pullRange(PChar);
            const float trip  = (distance(PChar->loc.p, PMob->loc.p) + distance(PMob->loc.p, camp)) / kRunSpeed + kClaimSeconds;
            if (!safeToPull(PChar, trip))
            {
                if (!PChar->PAI->IsEngaged() && canAct(PChar) && speedUp(bot, packets, tick))
                {
                    return true;
                }
                walkTo(PChar, waitSpot(PMob->loc.p, camp, std::max(range, 3.0f)), 2.0f, tick);
                return true;
            }

            // Ready: close to range and open with whatever reaches; a melee-only puller walks up and swings.
            const float reach = range > 0.0f ? range : PChar->GetMeleeRange(PMob) * 0.8f;
            if (distance(PChar->loc.p, PMob->loc.p) > reach)
            {
                walkTo(PChar, PMob->loc.p, reach - 1.0f, tick);
                return true;
            }

            PChar->PAI->PathFind->Clear();
            PChar->PAI->PathFind->LookAt(PMob->loc.p);
            if (range > 0.0f ? openPull(bot, packets, PMob, tick) : (engage(bot, packets, PMob), true))
            {
                if (!pull->announced)
                {
                    pull->announced = true;
                    callout(bot, fmt::format("Pulling {}", prettyName(PMob->getName())), false);
                }
            }
            return true;
        }
        case Stage::Returning:
        {
            // A link gets put to sleep if the puller has a sleep; either way it keeps running and the tank takes them at camp.
            std::vector<CMobEntity*> links;
            PChar->loc.zone->ForEachMob(
                [&](CMobEntity* POther)
                {
                    if (POther != PMob && POther->isAlive() && POther->GetBattleTarget() == PChar && !asleep(POther))
                    {
                        links.push_back(POther);
                    }
                });
            if (!links.empty() && canAct(PChar) && sleepLink(bot, packets, links.front(), PMob, tick))
            {
                callout(bot, fmt::format("Link! Sleeping {}", prettyName(links.front()->getName())), false);
                return true;
            }

            if (PMob == nullptr || !PMob->isAlive())
            {
                finish(*pull, tick, "it died");
                return false;
            }
            if (PMob->GetBattleTarget() != nullptr && PMob->GetBattleTarget() != PChar)
            {
                finish(*pull, tick, "the tank has it");
                return false;
            }
            if (tick > pull->deadline)
            {
                finish(*pull, tick, "took too long getting back");
                return false;
            }

            if (PChar->PAI->IsEngaged())
            {
                disengage(bot, packets);
            }
            if (distance(PChar->loc.p, camp) > kCampReached)
            {
                walkTo(PChar, camp, kCampReached - 1.0f, tick);
                return true;
            }

            // Home: the party takes it from here.
            finish(*pull, tick, "back at camp");
            return false;
        }
    }
    return false;
}

} // namespace bots
