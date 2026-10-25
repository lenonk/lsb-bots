//
// What a bot does with its job: heals, buffs, songs, nukes, enfeebles, abilities and weapon skills.
//
//   Every action goes through the same client packet a player would send, so the
//   server's own checks (range, MP, recasts, claim) still apply. Attempts are
//   throttled per action so a refused one isn't retried every tick.
//

#include "bots.h"

#include "common/logging.h"
#include "common/utils.h"

#include "map/ability.h"
#include "map/ai/ai_container.h"
#include "map/ai/states/magic_state.h"
#include "map/ai/controllers/player_controller.h"
#include "map/entities/char_entity.h"
#include "map/mob_spell_container.h"
#include "map/entities/mob_entity.h"
#include "map/enmity_container.h"
#include "map/entities/pet_entity.h"
#include "map/items/item_weapon.h"
#include "map/party.h"
#include "map/recast_container.h"
#include "map/spell.h"
#include "map/status_effect.h"
#include "map/status_effect_container.h"
#include "map/utils/battleutils.h"
#include "map/utils/charutils.h"
#include "map/utils/petutils.h"
#include "map/utils/zoneutils.h"
#include "map/zone.h"
#include "common/xirand.h"
#include "map/weapon_skill.h"

#include <algorithm>
#include <cctype>
#include <array>
#include <initializer_list>
#include <tuple>
#include <list>
#include <map>
#include <vector>

namespace bots
{

namespace
{

using xi::Job;
using xi::StatusEffect;

constexpr float kSpellRange      = 20.0f;
constexpr uint8 kEmergencyHPP    = 40;
constexpr int64 kHateMargin      = 80; // percent of the holder's enmity a damage dealer stays under
constexpr uint8 kWhmCureHPP      = 75;
constexpr uint8 kHasteTankMPP    = 50; // a healer hastes the tank down to this
constexpr uint8 kHasteAllMPP     = 85; // and every melee above this
constexpr uint8 kStrangerCureMPP = 50; // a passer-by is cured only with this much MP left
constexpr uint8 kStrangerBuffMPP = 90; // and given Protect or Shell only with this

// Everyone in the party in this zone, the dead included.
auto partyMembersAll(CCharEntity* PChar) -> std::vector<CBattleEntity*>
{
    std::vector<CBattleEntity*> members{ PChar };
    if (PChar->PParty != nullptr)
    {
        for (auto* PMember : PChar->PParty->members)
        {
            if (PMember != PChar && PMember->getZone() == PChar->getZone())
            {
                members.push_back(PMember);
            }
        }
    }
    return members;
}

// Throttle keys: spells by id, abilities and weapon skills offset past them.
constexpr uint32 kAbilityKey = 0x10000;
constexpr uint32 kSkillKey   = 0x20000;
constexpr uint32 kShootKey   = 0x40000;
constexpr uint32 kTellKey    = 0x50000;
constexpr uint32 kKindKey    = 0x60000; // buffs on strangers, by effect
constexpr uint32 kRaisedKey  = 0x70000;
constexpr uint32 kItemKey    = 0x80000;

constexpr float kNoticeRange = 40.0f; // how far a bot notices someone it could help

constexpr auto kReleaseAfter = std::chrono::seconds(20);
constexpr auto kChainOpens   = std::chrono::seconds(3); // a skill this soon after the last only reopens
constexpr auto kChainHold    = std::chrono::seconds(6); // how long an opener is left for someone who can close it

auto tryCast(Bot& bot, PacketSystem& packets, const SpellID id, CBattleEntity* PTarget, const timer::duration cooldown, const timer::time_point tick) -> bool
{
    auto* PChar  = bot.entity();
    auto* PSpell = spell::GetSpell(id);

    if (PSpell == nullptr || !spell::CanUseSpell(PChar, PSpell) || PChar->health.mp < PSpell->getMPCost() ||
        PChar->PRecastContainer->HasRecast(RECAST_MAGIC, static_cast<Recast>(id), 0s) || distance(PChar->loc.p, PTarget->loc.p) > kSpellRange ||
        !throttle(bot, static_cast<uint32>(id), PTarget->id, cooldown, tick))
    {
        return false;
    }

    castSpell(bot, packets, id, PTarget);
    return true;
}

// Highest tier first; the first one the bot can cast goes.
auto castBest(Bot& bot, PacketSystem& packets, const std::initializer_list<SpellID> ladder, CBattleEntity* PTarget, const timer::duration cooldown, const timer::time_point tick) -> bool
{
    return std::ranges::any_of(ladder,
                               [&](const SpellID id)
                               {
                                   return tryCast(bot, packets, id, PTarget, cooldown, tick);
                               });
}

auto tryAbility(Bot& bot, PacketSystem& packets, const uint16 id, CBattleEntity* PTarget, const timer::time_point tick) -> bool
{
    auto* PChar    = bot.entity();
    auto* PAbility = ability::GetAbility(id);

    if (PAbility == nullptr || !charutils::hasAbility(PChar, id) || PChar->PRecastContainer->HasRecast(RECAST_ABILITY, PAbility->getRecastId(), 0s) ||
        !throttle(bot, kAbilityKey | id, PTarget->id, std::chrono::seconds(5), tick))
    {
        return false;
    }

    useAbility(bot, packets, id, PTarget);
    return true;
}

struct SleepTries
{
    uint8             casts{};
    timer::time_point last{};
};

// Sleep casts per monster, shared by every bot: reset when one lands, given up after a few resists.
std::map<uint32, SleepTries> sleepTries;

constexpr uint8 kSleepTries  = 3;
constexpr float kHordeRadius = 10.0f; // Horde Lullaby this close to another monster would put it to sleep too
constexpr uint8 kNotWorthHPP = 25;    // an add this low is killed, not slept

enum class SleepKind : uint8
{
    Single, // one add, or one next to the monster being fought
    Area,   // several adds together, clear of the monster being fought
};

auto trySleep(Bot& bot, PacketSystem& packets, CBattleEntity* PMob, const SleepKind kind, const timer::time_point tick) -> bool
{
    if (sleepTries.size() > 256)
    {
        std::erase_if(sleepTries, [&](const auto& entry) { return tick - entry.second.last > std::chrono::minutes(5); });
    }

    auto& tries = sleepTries[PMob->id];
    if (tick - tries.last > std::chrono::minutes(2))
    {
        tries = {};
    }
    if (tries.casts >= kSleepTries)
    {
        return false;
    }

    const auto cooldown = std::chrono::seconds(3);
    const bool cast     = kind == SleepKind::Area
                              ? castBest(bot, packets, { SpellID::Horde_Lullaby, SpellID::Sleepga_II, SpellID::Sleepga, SpellID::Foe_Lullaby, SpellID::Sleep_II, SpellID::Sleep },
                                         PMob, cooldown, tick)
                              : castBest(bot, packets, { SpellID::Foe_Lullaby, SpellID::Sleep_II, SpellID::Sleep }, PMob, cooldown, tick);
    if (cast)
    {
        ++tries.casts;
        tries.last = tick;
    }
    return cast;
}

auto canSleep(CCharEntity* PChar) -> bool
{
    return std::ranges::any_of(std::array{ SpellID::Horde_Lullaby, SpellID::Foe_Lullaby, SpellID::Sleepga, SpellID::Sleep_II, SpellID::Sleep },
                               [&](const SpellID id) { return spell::CanUseSpell(PChar, id); });
}

// An awake add on the party worth sleeping: not this bot's target or the tank's, not being pulled, not out of tries.
auto findAdd(CCharEntity* PChar, CBattleEntity* PTarget, std::size_t* groupSize = nullptr) -> CBattleEntity*
{
    if (PChar->PParty == nullptr || !canSleep(PChar))
    {
        return nullptr;
    }

    auto*                       PTank    = mainTank(PChar);
    const auto*                 PTanking = PTank != nullptr ? PTank->GetBattleTarget() : nullptr;
    std::vector<CBattleEntity*> adds;
    PChar->loc.zone->ForEachMob(
        [&](CMobEntity* PMob)
        {
            if (!PMob->isAlive() || !PMob->PAI->IsEngaged() || PMob == PTarget || PMob == PTanking || PMob->GetBattleTarget() == nullptr ||
                PMob->GetBattleTarget()->PParty != PChar->PParty || beingPulled(PChar, PMob) || distance(PChar->loc.p, PMob->loc.p) > kSpellRange)
            {
                return;
            }
            if (asleep(PMob))
            {
                sleepTries[PMob->id].casts = 0; // it landed
                return;
            }
            if (PMob->GetHPP() >= kNotWorthHPP && sleepTries[PMob->id].casts < kSleepTries)
            {
                adds.push_back(PMob);
            }
        });

    // The one with the most others around it, so an area sleep catches the lot.
    CBattleEntity* PAdd    = nullptr;
    std::size_t    bestNear = 0;
    for (auto* PCandidate : adds)
    {
        const auto near = static_cast<std::size_t>(
            std::ranges::count_if(adds, [&](const CBattleEntity* POther) { return distance(POther->loc.p, PCandidate->loc.p) < kHordeRadius; }));
        if (PAdd == nullptr || near > bestNear)
        {
            PAdd     = PCandidate;
            bestNear = near;
        }
    }
    if (groupSize != nullptr)
    {
        *groupSize = bestNear;
    }
    return PAdd;
}

// Adds on the party go to sleep while the one being fought dies.
auto sleepAdds(Bot& bot, PacketSystem& packets, CBattleEntity* PTarget, const timer::time_point tick) -> bool
{
    std::size_t group = 0;
    auto*       PAdd  = findAdd(bot.entity(), PTarget, &group);
    if (PAdd == nullptr)
    {
        return false;
    }

    // Area sleep for two or more together, unless it would catch the monster being fought.
    const bool area = group >= 2 && distance(PAdd->loc.p, PTarget->loc.p) >= kHordeRadius;
    return trySleep(bot, packets, PAdd, area ? SleepKind::Area : SleepKind::Single, tick);
}

// "thunder_iii" -> "Thunder III", for callouts.
auto pretty(std::string name) -> std::string
{
    bool start = true;
    for (std::size_t i = 0; i < name.size(); ++i)
    {
        if (name[i] == '_')
        {
            name[i] = ' ';
            start   = true;
            continue;
        }
        if (start)
        {
            const auto end   = name.find('_', i);
            const auto word  = name.substr(i, end == std::string::npos ? std::string::npos : end - i);
            const bool roman = word.find_first_not_of("iv") == std::string::npos;
            for (std::size_t j = i; j < i + word.size(); ++j)
            {
                if (roman || j == i)
                {
                    name[j] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[j])));
                }
            }
            start = false;
        }
    }
    return name;
}

// The skillchain state on a target: an opener (tier 0) waiting for a closer, or a chain that has formed.
auto chainOn(const CBattleEntity* PTarget) -> CStatusEffect*
{
    return PTarget->StatusEffectContainer->GetStatusEffect(StatusEffect::Skillchain, 0);
}

auto chainsWith(const CStatusEffect* PChain, const CWeaponSkill* PSkill) -> bool
{
    const auto power = PChain->GetPower();

    std::list<SKILLCHAIN_ELEMENT> resonance;
    if (PChain->GetTier() == 0)
    {
        resonance = { static_cast<SKILLCHAIN_ELEMENT>(power & 0xF), static_cast<SKILLCHAIN_ELEMENT>((power >> 4) & 0xF),
                      static_cast<SKILLCHAIN_ELEMENT>((power >> 8) & 0xF) };
    }
    else
    {
        resonance = { static_cast<SKILLCHAIN_ELEMENT>(power) };
    }

    return battleutils::FormSkillchain(resonance, { static_cast<SKILLCHAIN_ELEMENT>(PSkill->getPrimarySkillchain()),
                                                    static_cast<SKILLCHAIN_ELEMENT>(PSkill->getSecondarySkillchain()),
                                                    static_cast<SKILLCHAIN_ELEMENT>(PSkill->getTertiarySkillchain()) }) != SC_NONE;
}

// Cumulative plus volatile enmity: what the monster compares to pick its target.
auto enmityOn(CBattleEntity* PTarget, CBattleEntity* PEntity) -> int64
{
    auto* PMob = dynamic_cast<CMobEntity*>(PTarget);
    if (PMob == nullptr || PMob->PEnmityContainer == nullptr)
    {
        return 0;
    }
    return static_cast<int64>(PMob->PEnmityContainer->GetCE(PEntity)) + PMob->PEnmityContainer->GetVE(PEntity);
}

auto volatileOn(CBattleEntity* PTarget, CBattleEntity* PEntity) -> int64
{
    auto* PMob = dynamic_cast<CMobEntity*>(PTarget);
    return PMob != nullptr && PMob->PEnmityContainer != nullptr ? PMob->PEnmityContainer->GetVE(PEntity) : 0;
}

// Damage that would bring a damage dealer's enmity this close to whoever holds the monster waits until they pull ahead.
// Only the main tank never holds back, and a party without one still keeps off whoever has it.
auto nearHate(CCharEntity* PChar, CBattleEntity* PTarget) -> bool
{
    auto* PHolder = PTarget->GetBattleTarget();
    if (PHolder == nullptr || PHolder == PChar || mainTank(PChar) == PChar)
    {
        return false;
    }
    return enmityOn(PTarget, PChar) * 100 >= enmityOn(PTarget, PHolder) * kHateMargin;
}

constexpr std::size_t kNukeRatios = 5;    // recent nukes the estimate learns from
constexpr float       kBurstHate  = 1.5f; // a magic burst's damage over a plain nuke's

// What a nuke would add to this bot's enmity: LSB counts damage as 80 CE and 240 VE over the monster's level divisor,
// and the damage is the spell's base plus its multiplier on the INT gap.
auto nukeEstimate(CCharEntity* PChar, CBattleEntity* PTarget, const CSpell* PSpell) -> int64
{
    const auto gap    = std::max(0, static_cast<int>(PChar->INT()) - static_cast<int>(PTarget->INT()));
    const auto damage = static_cast<float>(PSpell->getBase()) + PSpell->getMultiplier() * static_cast<float>(gap);
    return static_cast<int64>(320.0f * damage / static_cast<float>(battleutils::GetEnmityModDamage(PTarget->GetMLevel())));
}

// The estimate scaled to what this bot's nukes have really added (gear, merits, resists): the largest of the last few.
auto nukeScale(const Bot& bot) -> float
{
    return bot.nukeRatios.empty() ? 1.25f : *std::ranges::max_element(bot.nukeRatios);
}

// A nuke that keeps this bot under the margin of whoever holds the monster, counting the enmity it would add.
auto nukeFits(const Bot& bot, CCharEntity* PChar, CBattleEntity* PTarget, const CSpell* PSpell, const float burst) -> bool
{
    auto* PHolder = PTarget->GetBattleTarget();
    if (PHolder == nullptr || PHolder == PChar || mainTank(PChar) == PChar)
    {
        return true;
    }
    const auto added = static_cast<int64>(static_cast<float>(nukeEstimate(PChar, PTarget, PSpell)) * nukeScale(bot) * burst);
    return (enmityOn(PTarget, PChar) + added) * 100 < enmityOn(PTarget, PHolder) * kHateMargin;
}

auto castNuke(Bot& bot, PacketSystem& packets, const SpellID id, CBattleEntity* PTarget, const float burst, const timer::time_point tick) -> bool
{
    auto*       PChar  = bot.entity();
    const auto* PSpell = spell::GetSpell(id);
    if (PSpell == nullptr || !nukeFits(bot, PChar, PTarget, PSpell, burst))
    {
        return false;
    }

    const auto before   = enmityOn(PTarget, PChar);
    const auto beforeVE = volatileOn(PTarget, PChar);
    if (!tryCast(bot, packets, id, PTarget, 0s, tick))
    {
        return false;
    }
    bot.nukeWatch = NukeWatch{ EntityId(PTarget), id, before, beforeVE, static_cast<int64>(static_cast<float>(nukeEstimate(PChar, PTarget, PSpell)) * burst), tick };
    return true;
}

// The last nuke has landed: what it added against the estimate goes into the scale. One that took the monster off
// the main tank is logged, so the margin can be checked against real fights.
void learnNuke(Bot& bot, CCharEntity* PChar, const timer::time_point tick)
{
    if (!bot.nukeWatch)
    {
        return;
    }
    const auto watch = *bot.nukeWatch;
    bot.nukeWatch.reset();

    auto* PTarget = watch.target.resolve<CBattleEntity>();
    if (PTarget == nullptr || PTarget->isDead() || watch.estimate <= 0)
    {
        return;
    }

    // The volatile part lost while it was cast goes back on: 60 a second.
    const auto seconds = std::chrono::duration<float>(tick - watch.castAt).count();
    const auto decayed = std::min(watch.beforeVE, static_cast<int64>(60.0f * seconds));
    const auto added   = enmityOn(PTarget, PChar) - watch.before + decayed;
    if (added <= 0)
    {
        return; // interrupted, or it missed
    }

    bot.nukeRatios.push_back(static_cast<float>(added) / static_cast<float>(watch.estimate));
    if (bot.nukeRatios.size() > kNukeRatios)
    {
        bot.nukeRatios.pop_front();
    }

    auto* PTank = mainTank(PChar);
    if (PTarget->GetBattleTarget() == PChar && PTank != nullptr && PTank != PChar)
    {
        const auto* PSpell = spell::GetSpell(watch.spell);
        ShowInfoFmt("bots: {} took {} off {} with {}: added {} enmity, estimated {} (scale now {:.2f})", PChar->getName(), PTarget->getName(), PTank->getName(),
                    PSpell != nullptr ? const_cast<CSpell*>(PSpell)->getName() : std::string("a nuke"), added, watch.estimate, nukeScale(bot));
    }
}

auto holdDamage(CCharEntity* PChar, CBattleEntity* PTarget) -> bool
{
    return pulledHate(PChar, PTarget) || nearHate(PChar, PTarget);
}

auto tryWeaponSkill(Bot& bot, PacketSystem& packets, CBattleEntity* PTarget, const timer::time_point tick) -> bool
{
    auto* PChar = bot.entity();
    if (holdDamage(PChar, PTarget))
    {
        return false;
    }
    if (PChar->health.tp < 1000 && !PChar->StatusEffectContainer->HasStatusEffect(StatusEffect::MeikyoShisui))
    {
        return false;
    }

    const auto* PWeapon = dynamic_cast<CItemWeapon*>(isRanged(PChar) ? PChar->getEquip(SLOT_RANGED) : PChar->m_Weapons[SLOT_MAIN]);
    const auto  skill   = PWeapon ? PWeapon->getSkillType() : xi::SkillType::HandToHand;

    // The list runs from the lowest skill requirement up, so the last known one is the strongest.
    std::vector<const CWeaponSkill*> known;
    for (const auto* PSkill : battleutils::GetWeaponSkills(static_cast<uint8>(skill)))
    {
        if (charutils::hasWeaponSkill(PChar, PSkill->getID()))
        {
            known.push_back(PSkill);
        }
    }
    if (known.empty())
    {
        return false;
    }

    // Close an opener or extend a chain when a known skill links; the window opens 3 s after the last skill.
    // Otherwise leave the opener a few seconds for someone who can close it, and never step on a chain's burst.
    const CWeaponSkill* PBest = known.back();
    if (const auto* PChain = chainOn(PTarget))
    {
        const auto age = timer::now() - PChain->GetStartTime();
        if (age < kChainOpens)
        {
            return false;
        }

        const auto link = std::find_if(known.rbegin(), known.rend(), [&](const CWeaponSkill* PSkill) { return chainsWith(PChain, PSkill); });
        if (link != known.rend())
        {
            PBest = *link;
        }
        else if (PChain->GetTier() > 0 || age < kChainHold)
        {
            return false;
        }
    }

    if (PBest == nullptr || !throttle(bot, kSkillKey | PBest->getID(), PTarget->id, std::chrono::seconds(2), tick))
    {
        return false;
    }

    weaponSkill(bot, packets, PBest->getID(), PTarget);

    const auto* PChain = chainOn(PTarget);
    const auto  name   = pretty(const_cast<CWeaponSkill*>(PBest)->getName());
    callout(bot, PChain != nullptr && chainsWith(PChain, PBest) ? fmt::format("{} - closing!", name) : fmt::format("{} >> {}", name, pretty(PTarget->getName())), true);
    return true;
}

auto partyMembers(CCharEntity* PChar) -> std::vector<CBattleEntity*>
{
    std::vector<CBattleEntity*> members;
    if (PChar->PParty == nullptr)
    {
        members.push_back(PChar);
        return members;
    }

    for (auto* PMember : PChar->PParty->members)
    {
        if (PMember->getZone() == PChar->getZone() && !PMember->isDead() && distance(PChar->loc.p, PMember->loc.p) <= kSpellRange)
        {
            members.push_back(PMember);
        }
    }
    return members;
}

auto isSquishy(const Job job) -> bool
{
    switch (job)
    {
        case Job::WHM:
        case Job::BLM:
        case Job::RDM:
        case Job::BRD:
        case Job::SMN:
        case Job::SCH:
        case Job::GEO:
            return true;
        default:
            return false;
    }
}

auto heal(Bot& bot, PacketSystem& packets, const std::vector<CBattleEntity*>& members, const uint8 threshold, const timer::time_point tick) -> bool
{
    CBattleEntity* PWorst = nullptr;
    for (auto* PMember : members)
    {
        if (PMember->GetHPP() < threshold && (PWorst == nullptr || PMember->GetHPP() < PWorst->GetHPP()))
        {
            PWorst = PMember;
        }
    }

    if (PWorst == nullptr)
    {
        return false;
    }

    // The cheapest cure that gets away with it: two casts are fine when they're in no danger, one when they are.
    static constexpr std::array<SpellID, 4> cures  = { SpellID::Cure, SpellID::Cure_II, SpellID::Cure_III, SpellID::Cure_IV };
    static constexpr std::array<int32, 4>   amount = { 40, 100, 220, 450 }; // rough heal per tier

    const auto missing = PWorst->GetMaxHP() - PWorst->health.hp;
    const auto needed  = PWorst->GetHPP() >= 50 ? (missing + 1) / 2 : missing * 7 / 10;

    std::size_t tier = 0;
    while (tier + 1 < cures.size() && amount[tier] < needed)
    {
        ++tier;
    }

    // That tier, then cheaper ones, then bigger ones if those aren't available yet.
    const auto cooldown = std::chrono::seconds(3);
    for (std::size_t i = tier + 1; i-- > 0;)
    {
        if (tryCast(bot, packets, cures[i], PWorst, cooldown, tick))
        {
            return true;
        }
    }
    for (auto i = tier + 1; i < cures.size(); ++i)
    {
        if (tryCast(bot, packets, cures[i], PWorst, cooldown, tick))
        {
            return true;
        }
    }
    return false;
}

auto buff(Bot& bot, PacketSystem& packets, const std::vector<CBattleEntity*>& members, const StatusEffect effect, const std::initializer_list<SpellID> ladder, const timer::time_point tick) -> bool
{
    return std::ranges::any_of(members,
                               [&](CBattleEntity* PMember)
                               {
                                   return !PMember->StatusEffectContainer->HasStatusEffect(effect) && castBest(bot, packets, ladder, PMember, std::chrono::seconds(30), tick);
                               });
}

struct EnfeebleTries
{
    uint8             casts{};
    timer::time_point last{};
};

// Enfeeble casts per monster and effect, shared by the party: reset when one lands, given up after a few resists.
std::map<uint64, EnfeebleTries> enfeebleTries;

constexpr uint8 kEnfeebleTries = 3;

auto enfeeble(Bot& bot, PacketSystem& packets, CBattleEntity* PTarget, const StatusEffect effect, const std::initializer_list<SpellID> ladder, const timer::time_point tick) -> bool
{
    if (enfeebleTries.size() > 512)
    {
        std::erase_if(enfeebleTries, [&](const auto& entry) { return tick - entry.second.last > std::chrono::minutes(5); });
    }

    auto& tries = enfeebleTries[(static_cast<uint64>(PTarget->id) << 16) | static_cast<uint16>(effect)];
    if (PTarget->StatusEffectContainer->HasStatusEffect(effect))
    {
        tries.casts = 0;
        return false;
    }
    if (tick - tries.last > std::chrono::minutes(2))
    {
        tries = {};
    }
    if (tries.casts >= kEnfeebleTries)
    {
        return false;
    }

    if (castBest(bot, packets, ladder, PTarget, std::chrono::seconds(8), tick))
    {
        ++tries.casts;
        tries.last = tick;
        return true;
    }
    return false;
}

auto partyHas(CCharEntity* PChar, const Job job) -> bool
{
    return PChar->PParty != nullptr && std::ranges::any_of(PChar->PParty->members,
                                                           [&](const CBattleEntity* PMember) { return !PMember->isDead() && PMember->GetMJob() == job; });
}

// Buffs a monster puts up that Dispel takes off.
auto buffed(CBattleEntity* PTarget) -> bool
{
    return PTarget->StatusEffectContainer->HasStatusEffect({ StatusEffect::Protect, StatusEffect::Shell, StatusEffect::Haste, StatusEffect::Blink,
                                                             StatusEffect::Stoneskin, StatusEffect::Regen, StatusEffect::BlazeSpikes, StatusEffect::IceSpikes,
                                                             StatusEffect::ShockSpikes, StatusEffect::AttackBoost, StatusEffect::DefenseBoost,
                                                             StatusEffect::MagicAtkBoost, StatusEffect::MagicDefBoost, StatusEffect::Berserk,
                                                             StatusEffect::Defender, StatusEffect::Phalanx });
}

// The red mage's list: Silence a caster first, strip its buffs, then Dia (never Bio over it), Slow, Paralyze,
// and Blind on monsters that only melee. Highest tier first; Dia III and the II's come at 75.
auto debuff(Bot& bot, PacketSystem& packets, CBattleEntity* PTarget, const timer::time_point tick) -> bool
{
    const auto* PMob   = dynamic_cast<CMobEntity*>(PTarget);
    const bool  caster = PMob != nullptr && PMob->SpellContainer != nullptr && PMob->SpellContainer->HasSpells();

    if (caster && enfeeble(bot, packets, PTarget, StatusEffect::Silence, { SpellID::Silence }, tick))
    {
        return true;
    }
    if (buffed(PTarget) && tryCast(bot, packets, SpellID::Dispel, PTarget, std::chrono::seconds(10), tick))
    {
        return true;
    }
    if (!PTarget->StatusEffectContainer->HasStatusEffect(StatusEffect::Bio) &&
        enfeeble(bot, packets, PTarget, StatusEffect::Dia, { SpellID::Dia_III, SpellID::Dia_II, SpellID::Dia }, tick))
    {
        return true;
    }
    return enfeeble(bot, packets, PTarget, StatusEffect::Slow, { SpellID::Slow_II, SpellID::Slow }, tick) ||
           enfeeble(bot, packets, PTarget, StatusEffect::Paralysis, { SpellID::Paralyze_II, SpellID::Paralyze }, tick) ||
           (!caster && enfeeble(bot, packets, PTarget, StatusEffect::Blindness, { SpellID::Blind_II, SpellID::Blind }, tick));
}

constexpr SpellID kNukes[] = { SpellID::Thunder_III, SpellID::Blizzard_III, SpellID::Fire_III, SpellID::Aero_III, SpellID::Water_III, SpellID::Stone_III,
                               SpellID::Thunder_II,  SpellID::Blizzard_II,  SpellID::Fire_II,  SpellID::Aero_II,  SpellID::Water_II,  SpellID::Stone_II,
                               SpellID::Thunder,     SpellID::Blizzard,     SpellID::Fire,     SpellID::Aero,     SpellID::Water,     SpellID::Stone };

// Each nuke is the strongest whose enmity keeps the bot under whoever holds the monster, a burst included; when even
// the weakest would pull it, the bot waits for the holder to get further ahead.
auto nuke(Bot& bot, PacketSystem& packets, CBattleEntity* PTarget, const timer::duration cooldown, const timer::time_point tick) -> bool
{
    if (pulledHate(bot.entity(), PTarget))
    {
        return false;
    }

    // A formed skillchain: burst it straight away with a matching element.
    if (const auto* PChain = chainOn(PTarget); PChain != nullptr && PChain->GetTier() > 0)
    {
        const auto elements = battleutils::GetSkillchainMagicElement(static_cast<SKILLCHAIN_ELEMENT>(PChain->GetPower()));
        for (const auto id : kNukes)
        {
            const auto* PSpell = spell::GetSpell(id);
            if (PSpell != nullptr && std::ranges::find(elements, static_cast<ELEMENT>(PSpell->getElement())) != elements.end() &&
                castNuke(bot, packets, id, PTarget, kBurstHate, tick))
            {
                callout(bot, fmt::format("Magic Burst: {}!", pretty(const_cast<CSpell*>(PSpell)->getName())), true);
                return true;
            }
        }
    }

    // Let whoever holds hate keep it for a moment before the first nuke.
    if (PTarget->GetHPP() > 95 || !throttle(bot, 0xFFFF, PTarget->id, cooldown, tick))
    {
        return false;
    }

    return std::ranges::any_of(kNukes, [&](const SpellID id) { return castNuke(bot, packets, id, PTarget, 1.0f, tick); });
}

auto sing(Bot& bot, PacketSystem& packets, const std::vector<CBattleEntity*>& members, const timer::time_point tick) -> bool
{
    auto* PChar     = bot.entity();
    const bool mages = std::ranges::any_of(members, [](const CBattleEntity* PMember) { return PMember->GetMaxMP() > 0 && isSquishy(PMember->GetMJob()); });

    if (!PChar->StatusEffectContainer->HasStatusEffect(StatusEffect::Minuet) &&
        castBest(bot, packets, { SpellID::Valor_Minuet_III, SpellID::Valor_Minuet_II, SpellID::Valor_Minuet }, PChar, std::chrono::seconds(10), tick))
    {
        return true;
    }

    // A bard holds two songs; a third would knock the oldest off. Ballad for mages, March (Paeon before it's learnt) otherwise.
    if (mages)
    {
        return !PChar->StatusEffectContainer->HasStatusEffect(StatusEffect::Ballad) &&
               castBest(bot, packets, { SpellID::Mages_Ballad_II, SpellID::Mages_Ballad }, PChar, std::chrono::seconds(10), tick);
    }

    return !PChar->StatusEffectContainer->HasStatusEffect(StatusEffect::March) && !PChar->StatusEffectContainer->HasStatusEffect(StatusEffect::Paeon) &&
           castBest(bot, packets, { SpellID::Victory_March, SpellID::Advancing_March, SpellID::Armys_Paeon_III, SpellID::Armys_Paeon_II, SpellID::Armys_Paeon },
                    PChar, std::chrono::seconds(10), tick);
}

struct Cleanse
{
    StatusEffect effect;
    SpellID      spell;
};

// Ailments a healer lifts, worst first. A cure wakes the sleeping.
constexpr Cleanse kCleanses[] = {
    { StatusEffect::Doom, SpellID::Cursna },          { StatusEffect::SleepI, SpellID::Cure },     { StatusEffect::SleepIi, SpellID::Cure },
    { StatusEffect::Lullaby, SpellID::Cure },         { StatusEffect::Petrification, SpellID::Stona }, { StatusEffect::Silence, SpellID::Silena },
    { StatusEffect::Paralysis, SpellID::Paralyna },   { StatusEffect::CurseI, SpellID::Cursna },   { StatusEffect::CurseIi, SpellID::Cursna },
    { StatusEffect::Blindness, SpellID::Blindna },    { StatusEffect::Poison, SpellID::Poisona },  { StatusEffect::Disease, SpellID::Viruna },
    { StatusEffect::Plague, SpellID::Viruna },        { StatusEffect::Slow, SpellID::Erase },      { StatusEffect::Weight, SpellID::Erase },
    { StatusEffect::Bind, SpellID::Erase },
};

// White Mages take on every ailment; other healers only wake the sleeping.
auto cleanse(Bot& bot, PacketSystem& packets, const std::vector<CBattleEntity*>& members, const bool everything, const timer::time_point tick) -> bool
{
    for (const auto& [effect, spell] : kCleanses)
    {
        if (!everything && spell != SpellID::Cure)
        {
            continue;
        }
        for (auto* PMember : members)
        {
            if (PMember->StatusEffectContainer->HasStatusEffect(effect) && tryCast(bot, packets, spell, PMember, std::chrono::seconds(5), tick))
            {
                return true;
            }
        }
    }
    return false;
}

// Refresh on anyone with MP, Regen on the tank, Protect and Shell on everyone (in a fight too), Haste on the melee.
// Protect and Shell are the White Mage's; a red mage or scholar covers them only without one.
auto support(Bot& bot, PacketSystem& packets, const std::vector<CBattleEntity*>& members, CBattleEntity* PTarget, const timer::time_point tick) -> bool
{
    auto*      PChar  = bot.entity();
    const auto job    = PChar->GetMJob();
    const bool covers = job == Job::WHM || ((job == Job::RDM || job == Job::SCH) && !partyHas(PChar, Job::WHM));

    std::vector<CBattleEntity*> melee;
    std::vector<CBattleEntity*> mana;
    for (auto* PMember : members)
    {
        if (!isSquishy(PMember->GetMJob()))
        {
            melee.push_back(PMember);
        }
        if (PMember->GetMaxMP() > 0)
        {
            mana.push_back(PMember);
        }
    }

    if (job == Job::RDM && buff(bot, packets, mana, StatusEffect::Refresh, { SpellID::Refresh }, tick))
    {
        return true;
    }

    // Regen stays on the main tank all fight, or on whoever is being hit without one; red mages don't get it this era.
    if (job == Job::WHM && PTarget != nullptr)
    {
        CBattleEntity* PRegen = mainTank(PChar);
        if (PRegen == nullptr || std::ranges::find(members, PRegen) == members.end())
        {
            PRegen = PTarget->GetBattleTarget();
        }
        if (PRegen != nullptr && std::ranges::find(members, PRegen) != members.end() && !PRegen->StatusEffectContainer->HasStatusEffect(StatusEffect::Regen) &&
            castBest(bot, packets, { SpellID::Regen_II, SpellID::Regen }, PRegen, std::chrono::seconds(20), tick))
        {
            return true;
        }
    }

    if (covers && (buff(bot, packets, members, StatusEffect::Protect, { SpellID::Protect_III, SpellID::Protect_II, SpellID::Protect }, tick) ||
                   buff(bot, packets, members, StatusEffect::Shell, { SpellID::Shell_III, SpellID::Shell_II, SpellID::Shell }, tick)))
    {
        return true;
    }

    if (PTarget == nullptr || (job != Job::WHM && job != Job::RDM))
    {
        return false;
    }

    // Haste is 40 MP: the main tank (or whoever the monster is on) gets it while half the MP is left, the other melee
    // only with plenty to spare.
    std::vector<CBattleEntity*> hasted;
    if (PChar->GetMPP() >= kHasteAllMPP)
    {
        hasted = std::move(melee);
    }
    else if (PChar->GetMPP() >= kHasteTankMPP)
    {
        auto* PFront = mainTank(PChar);
        if (PFront == nullptr)
        {
            PFront = PTarget->GetBattleTarget();
        }
        if (std::ranges::find(melee, PFront) != melee.end())
        {
            hasted.push_back(PFront);
        }
    }
    return buff(bot, packets, hasted, StatusEffect::Haste, { SpellID::Haste }, tick);
}

// A notorious monster, three or more awake on the party, or the tank about to fall with the monster still fresh.
auto desperate(CCharEntity* PChar, CBattleEntity* PTarget) -> bool
{
    auto* PMob = dynamic_cast<CMobEntity*>(PTarget);
    if (PMob == nullptr)
    {
        return false;
    }
    if ((PMob->m_Type & xi::MobType::Notorious) != xi::MobType::Normal && PMob->GetHPP() > 20)
    {
        return true;
    }

    int awake = 0;
    PChar->loc.zone->ForEachMob(
        [&](CMobEntity* POther)
        {
            auto* PVictim = POther->GetBattleTarget();
            if (POther->isAlive() && POther->PAI->IsEngaged() && PVictim != nullptr && (PVictim == PChar || (PChar->PParty != nullptr && PVictim->PParty == PChar->PParty)) &&
                !asleep(POther))
            {
                ++awake;
            }
        });
    if (awake >= 3)
    {
        return true;
    }

    const auto* PTank = mainTank(PChar);
    const auto* PHurt = PTank != nullptr ? PTank : PMob->GetBattleTarget();
    return PHurt != nullptr && PHurt->GetHPP() < 30 && PMob->GetHPP() > 40;
}

// Scholar arts and Sublimation, Corsair rolls, Geomancer colures: the buffs a job keeps going between fights and in them.
auto upkeep(Bot& bot, PacketSystem& packets, const std::vector<CBattleEntity*>& members, CBattleEntity* PTarget, const timer::time_point tick) -> bool
{
    auto*      PChar  = bot.entity();
    const auto job    = PChar->GetMJob();
    const auto self   = [&](const uint16 id) { return tryAbility(bot, packets, id, PChar, tick); };
    const auto effect = [&](const StatusEffect id) { return PChar->StatusEffectContainer->HasStatusEffect(id); };
    const auto casters =
        std::ranges::count_if(members, [](const CBattleEntity* PMember) { return isCaster(PMember->GetMJob()) || PMember->GetMJob() == Job::RDM; });
    const bool town = (static_cast<uint16>(PChar->loc.zone->GetTypeMask()) & static_cast<uint16>(xi::ZoneType::City)) != 0;

    if (job == Job::SCH)
    {
        // The healer's arts when there's no White Mage to heal, the nuker's otherwise.
        if (!effect(StatusEffect::LightArts) && !effect(StatusEffect::DarkArts) && self(partyHas(PChar, Job::WHM) ? ABILITY_DARK_ARTS : ABILITY_LIGHT_ARTS))
        {
            return true;
        }
        // Sublimation charges on HP, then pays out as MP.
        return (!effect(StatusEffect::SublimationActivated) && !effect(StatusEffect::SublimationComplete) && PChar->GetHPP() > 60 && self(ABILITY_SUBLIMATION)) ||
               (effect(StatusEffect::SublimationComplete) && PChar->GetMPP() < 75 && self(ABILITY_SUBLIMATION));
    }

    if (job == Job::GEO && !town)
    {
        // Indi- on itself always: Refresh for a caster-heavy party, else attack, defence, regen, accuracy.
        if (!effect(StatusEffect::ColureActive) &&
            (casters >= 2 ? castBest(bot, packets, { SpellID::Indi_Refresh, SpellID::Indi_Fury, SpellID::Indi_Barrier, SpellID::Indi_Regen, SpellID::Indi_Precision },
                                     PChar, std::chrono::seconds(10), tick)
                          : castBest(bot, packets, { SpellID::Indi_Fury, SpellID::Indi_Barrier, SpellID::Indi_Regen, SpellID::Indi_Precision, SpellID::Indi_Refresh },
                                     PChar, std::chrono::seconds(10), tick)))
        {
            return true;
        }
        // A luopan by the melee for each fight; Full Circle gives some MP back once the fight is over.
        if (PTarget == nullptr)
        {
            return PChar->PPet != nullptr && tick - bot.foughtAt > std::chrono::seconds(15) && self(ABILITY_FULL_CIRCLE);
        }
        auto* PTank = mainTank(PChar);
        auto* PBy   = PTank != nullptr && std::ranges::find(members, PTank) != members.end() ? PTank : PChar;
        return PChar->PPet == nullptr &&
               castBest(bot, packets, { SpellID::Geo_Fury, SpellID::Geo_Barrier, SpellID::Geo_Regen, SpellID::Geo_Precision }, PBy, std::chrono::seconds(10), tick);
    }

    if ((job != Job::COR && PChar->GetSJob() != Job::COR) || town)
    {
        return false;
    }

    // Double-Up while the total is low or unlucky, and never on a lucky one or 7 and up.
    if (const auto* PChance = PChar->StatusEffectContainer->GetStatusEffect(StatusEffect::DoubleUpChance))
    {
        static const std::map<StatusEffect, std::pair<uint16, uint16>> luck = {
            { StatusEffect::ChaosRoll, { 4, 8 } },   { StatusEffect::SamuraiRoll, { 2, 6 } }, { StatusEffect::FightersRoll, { 5, 9 } },
            { StatusEffect::HuntersRoll, { 4, 8 } }, { StatusEffect::CorsairsRoll, { 5, 9 } }, { StatusEffect::EvokersRoll, { 5, 9 } },
            { StatusEffect::WarlocksRoll, { 4, 8 } }, { StatusEffect::HealersRoll, { 3, 7 } },
        };
        const auto  rolled = static_cast<StatusEffect>(PChance->GetSubPower());
        const auto* PRoll  = PChar->StatusEffectContainer->GetStatusEffect(rolled);
        const auto  lucky  = luck.contains(rolled) ? luck.at(rolled) : std::pair<uint16, uint16>{ 0, 0 };
        if (PRoll != nullptr)
        {
            const auto total = PRoll->GetSubPower();
            if (total != lucky.first && (total <= 5 || total == lucky.second) && self(ABILITY_DOUBLE_UP))
            {
                return true;
            }
        }
        return false;
    }

    // Rolls reach 8 yalms, so only with most of the party gathered round.
    const auto near = std::ranges::count_if(members, [&](const CBattleEntity* PMember) { return distance(PChar->loc.p, PMember->loc.p) <= 7.0f; });
    if (near * 3 < static_cast<std::ptrdiff_t>(members.size()) * 2)
    {
        return false;
    }

    // Attack and Store TP for a melee party, Refresh and magic accuracy for a casting one; Corsair's for the experience.
    const bool melee = casters * 2 < static_cast<std::ptrdiff_t>(members.size());
    static constexpr std::array<std::pair<uint16, StatusEffect>, 5> meleeRolls = { { { ABILITY_CHAOS_ROLL, StatusEffect::ChaosRoll },
                                                                                     { ABILITY_SAMURAI_ROLL, StatusEffect::SamuraiRoll },
                                                                                     { ABILITY_FIGHTERS_ROLL, StatusEffect::FightersRoll },
                                                                                     { ABILITY_HUNTERS_ROLL, StatusEffect::HuntersRoll },
                                                                                     { ABILITY_CORSAIRS_ROLL, StatusEffect::CorsairsRoll } } };
    static constexpr std::array<std::pair<uint16, StatusEffect>, 5> magicRolls = { { { ABILITY_EVOKERS_ROLL, StatusEffect::EvokersRoll },
                                                                                     { ABILITY_WARLOCKS_ROLL, StatusEffect::WarlocksRoll },
                                                                                     { ABILITY_HEALERS_ROLL, StatusEffect::HealersRoll },
                                                                                     { ABILITY_CHAOS_ROLL, StatusEffect::ChaosRoll },
                                                                                     { ABILITY_CORSAIRS_ROLL, StatusEffect::CorsairsRoll } } };
    const auto& wanted = melee ? meleeRolls : magicRolls;
    const auto  most   = job == Job::COR ? 2 : 1;

    // The first rolls on the list it knows, up to how many it can hold.
    int up = 0;
    for (const auto& [ability, rollEffect] : wanted)
    {
        if (up >= most || !charutils::hasAbility(PChar, ability))
        {
            continue;
        }
        ++up;
        if (!effect(rollEffect) && !effect(StatusEffect::Bust) && self(ability))
        {
            return true;
        }
    }
    return false;
}

// Two-hour and other big abilities, saved for when they're needed.
auto emergency(Bot& bot, PacketSystem& packets, const std::vector<CBattleEntity*>& members, CBattleEntity* PTarget, const timer::time_point tick) -> bool
{
    auto* PChar = bot.entity();
    const auto announce = [&](const bool used, const char* what)
    {
        if (used)
        {
            callout(bot, what, false);
        }
        return used;
    };

    switch (PChar->GetMJob())
    {
        case Job::WHM:
            return announce(std::ranges::count_if(members, [](const CBattleEntity* PMember) { return PMember->GetHPP() < 35; }) >= 2 &&
                   tryAbility(bot, packets, ABILITY_BENEDICTION, PChar, tick), "Benediction!");
        case Job::PLD:
            return announce(PTarget != nullptr && PTarget->GetBattleTarget() == PChar && PChar->GetHPP() < 25 && tryAbility(bot, packets, ABILITY_INVINCIBLE, PChar, tick),
                            "Invincible!");
        case Job::BLM:
            return announce(PTarget != nullptr && PChar->GetMPP() < 15 && PTarget->GetHPP() > 50 && tryAbility(bot, packets, ABILITY_MANAFONT, PChar, tick), "Manafont!");
        case Job::RDM:
            return (PChar->GetMPP() < 25 && PChar->GetHPP() > 70 && tryAbility(bot, packets, ABILITY_CONVERT, PChar, tick)) ||
                   announce(PTarget != nullptr && desperate(PChar, PTarget) && tryAbility(bot, packets, ABILITY_CHAINSPELL, PChar, tick), "Chainspell!");
        case Job::THF:
            return announce(PTarget != nullptr && PTarget->GetBattleTarget() == PChar && PChar->GetHPP() < 40 && tryAbility(bot, packets, ABILITY_PERFECT_DODGE, PChar, tick),
                            "Perfect Dodge!");
        default:
            break;
    }

    if (PTarget == nullptr || !desperate(PChar, PTarget))
    {
        return false;
    }

    switch (PChar->GetMJob())
    {
        case Job::WAR:
            return announce(tryAbility(bot, packets, ABILITY_MIGHTY_STRIKES, PChar, tick), "Mighty Strikes!");
        case Job::MNK:
            return announce(tryAbility(bot, packets, ABILITY_HUNDRED_FISTS, PChar, tick), "Hundred Fists!");
        case Job::DRK:
            return announce(tryAbility(bot, packets, ABILITY_BLOOD_WEAPON, PChar, tick), "Blood Weapon!");
        case Job::SAM:
            return announce(tryAbility(bot, packets, ABILITY_MEIKYO_SHISUI, PChar, tick), "Meikyo Shisui!");
        case Job::RNG:
            return announce(isRanged(PChar) && tryAbility(bot, packets, ABILITY_EAGLE_EYE_SHOT, PTarget, tick), "Eagle Eye Shot!");
        case Job::DRG:
            return announce(PChar->PPet != nullptr && tryAbility(bot, packets, ABILITY_SPIRIT_SURGE, PChar, tick), "Spirit Surge!");
        case Job::BRD:
            return announce(tryAbility(bot, packets, ABILITY_SOUL_VOICE, PChar, tick), "Soul Voice!");
        case Job::BST:
            return announce(PChar->PPet != nullptr && tryAbility(bot, packets, ABILITY_FAMILIAR, PChar, tick), "Familiar!");
        default:
            return false;
    }
}

constexpr uint8 kTankHandOverHPP = 40;

auto shoot(Bot& bot, PacketSystem& packets, CBattleEntity* PTarget, const timer::time_point tick) -> bool
{
    auto* PChar = bot.entity();
    if (!isRanged(PChar) || holdDamage(PChar, PTarget) || distance(PChar->loc.p, PTarget->loc.p) > PChar->GetRangedAttackRange() ||
        !throttle(bot, kShootKey, PTarget->id, std::chrono::seconds(1), tick))
    {
        return false;
    }

    rangedAttack(bot, packets, PTarget);
    return true;
}

// Utsusemi whenever the shadows are gone, for a ninja main or sub.
auto shadows(Bot& bot, PacketSystem& packets, const timer::time_point tick) -> bool
{
    auto* PChar = bot.entity();
    if ((PChar->GetMJob() != Job::NIN && PChar->GetSJob() != Job::NIN) || PChar->StatusEffectContainer->HasStatusEffect(StatusEffect::CopyImage))
    {
        return false;
    }
    return castBest(bot, packets, { SpellID::Utsusemi_Ni, SpellID::Utsusemi_Ichi }, PChar, std::chrono::seconds(3), tick);
}

// Each avatar's strongest damaging pact first.
auto bloodPacts(const uint32 petId) -> std::vector<uint16>
{
    switch (petId)
    {
        case PETID_IFRIT: return { ABILITY_FLAMING_CRUSH, ABILITY_DOUBLE_PUNCH, ABILITY_PUNCH };
        case PETID_GARUDA: return { ABILITY_PREDATOR_CLAWS, ABILITY_CLAW };
        case PETID_TITAN: return { ABILITY_MEGALITH_THROW, ABILITY_ROCK_BUSTER, ABILITY_ROCK_THROW };
        case PETID_CARBUNCLE: return { ABILITY_METEORITE, ABILITY_POISON_NAILS };
        default: return {};
    }
}

// Avatars and wyverns: call one up for a fight, send it in, and let a summoner's go once things are quiet.
auto pets(Bot& bot, PacketSystem& packets, CBattleEntity* PTarget, const timer::time_point tick) -> bool
{
    auto* PChar = bot.entity();
    auto* PPet  = PChar->PPet;

    if (PChar->GetMJob() == Job::DRG)
    {
        return PPet == nullptr && tryAbility(bot, packets, ABILITY_CALL_WYVERN, PChar, tick);
    }

    // The automaton stays out; in a fight it's sent in, kept on three maneuvers and repaired when it's hurt.
    if (PChar->GetMJob() == Job::PUP)
    {
        const bool town = (static_cast<uint16>(PChar->loc.zone->GetTypeMask()) & static_cast<uint16>(xi::ZoneType::City)) != 0;
        if (PPet == nullptr)
        {
            return !town && tryAbility(bot, packets, ABILITY_ACTIVATE, PChar, tick);
        }
        if (PTarget == nullptr)
        {
            return false;
        }
        if (PPet->GetHPP() < 40 && PChar->getEquip(SLOT_AMMO) != nullptr && tryAbility(bot, packets, ABILITY_REPAIR, PChar, tick))
        {
            return true;
        }
        if (PPet->GetBattleTarget() != PTarget)
        {
            return tryAbility(bot, packets, ABILITY_DEPLOY, PTarget, tick);
        }
        if (PChar->StatusEffectContainer->HasStatusEffect(StatusEffect::Overload))
        {
            return false;
        }

        // Fire, Thunder and Wind for a melee frame: the one with the fewest up.
        static constexpr std::array<std::pair<StatusEffect, uint16>, 3> maneuvers = { { { StatusEffect::FireManeuver, ABILITY_FIRE_MANEUVER },
                                                                                        { StatusEffect::ThunderManeuver, ABILITY_THUNDER_MANEUVER },
                                                                                        { StatusEffect::WindManeuver, ABILITY_WIND_MANEUVER } } };
        std::size_t total = 0;
        auto        least = maneuvers.front();
        std::size_t fewest = 99;
        for (const auto& maneuver : maneuvers)
        {
            const auto count = static_cast<std::size_t>(PChar->StatusEffectContainer->GetStatusEffectCountInIDRange(maneuver.first, maneuver.first));
            total += count;
            if (count < fewest)
            {
                fewest = count;
                least  = maneuver;
            }
        }
        return total < 3 && tryAbility(bot, packets, least.second, PChar, tick);
    }

    // A jug pet for each fight; it stays out between them.
    if (PChar->GetMJob() == Job::BST)
    {
        if (PTarget == nullptr)
        {
            return false;
        }
        if (PPet == nullptr)
        {
            return PChar->getEquip(SLOT_AMMO) != nullptr && tryAbility(bot, packets, ABILITY_CALL_BEAST, PChar, tick);
        }
        return PPet->GetBattleTarget() != PTarget && tryAbility(bot, packets, ABILITY_FIGHT, PTarget, tick);
    }

    if (PChar->GetMJob() != Job::SMN)
    {
        return false;
    }

    if (PTarget == nullptr)
    {
        // Perpetuation eats MP, so the avatar goes home between fights.
        return PPet != nullptr && tick - bot.foughtAt > kReleaseAfter && tryAbility(bot, packets, ABILITY_RELEASE, PChar, tick);
    }

    if (PPet == nullptr)
    {
        return castBest(bot, packets, { SpellID::Ifrit, SpellID::Garuda, SpellID::Titan, SpellID::Carbuncle }, PChar, std::chrono::seconds(10), tick);
    }

    if (PPet->GetBattleTarget() != PTarget)
    {
        return tryAbility(bot, packets, ABILITY_ASSAULT, PTarget, tick);
    }

    const auto* PPetEntity = dynamic_cast<CPetEntity*>(PPet);
    if (PPetEntity == nullptr || PChar->GetMPP() < 30)
    {
        return false;
    }
    return std::ranges::any_of(bloodPacts(PPetEntity->petID()), [&](const uint16 id) { return tryAbility(bot, packets, id, PTarget, tick); });
}

auto isTank(const Job job) -> bool
{
    return job == Job::PLD || job == Job::WAR || job == Job::NIN || job == Job::RUN;
}

// A monster in the party beating on someone the tank should protect: a non-tank, or a tank getting low.
auto strayMonster(CCharEntity* PChar) -> CBattleEntity*
{
    CBattleEntity* PNearest = nullptr;
    PChar->loc.zone->ForEachMob(
        [&](CMobEntity* PMob)
        {
            auto* PVictim = PMob->GetBattleTarget();
            if (!PMob->isAlive() || !PMob->PAI->IsEngaged() || PVictim == nullptr || PVictim == PChar || PVictim->PParty != PChar->PParty ||
                distance(PChar->loc.p, PMob->loc.p) > kSpellRange || beingPulled(PChar, PMob) || asleep(PMob))
            {
                return;
            }
            if (isTank(PVictim->GetMJob()) && PVictim->GetHPP() >= kTankHandOverHPP)
            {
                return;
            }
            if (PNearest == nullptr || distance(PChar->loc.p, PMob->loc.p) < distance(PChar->loc.p, PNearest->loc.p))
            {
                PNearest = PMob;
            }
        });
    return PNearest;
}

auto grab(Bot& bot, PacketSystem& packets, CBattleEntity* PMob, const timer::time_point tick) -> bool
{
    if (tryAbility(bot, packets, ABILITY_PROVOKE, PMob, tick) || tryCast(bot, packets, SpellID::Flash, PMob, std::chrono::seconds(5), tick))
    {
        callout(bot, fmt::format("Provoke >> {}", pretty(PMob->getName())), true);
        return true;
    }
    return false;
}

auto provokeReady(CBattleEntity* PEntity) -> bool
{
    auto* PChar    = dynamic_cast<CCharEntity*>(PEntity);
    auto* PProvoke = ability::GetAbility(ABILITY_PROVOKE);
    return PChar != nullptr && PProvoke != nullptr && charutils::hasAbility(PChar, ABILITY_PROVOKE) &&
           !PChar->PRecastContainer->HasRecast(RECAST_ABILITY, PProvoke->getRecastId(), 0s);
}

// Tanks take hate from anyone but another tank, and from that tank too once it gets low.
auto tank(Bot& bot, PacketSystem& packets, CBattleEntity* PTarget, const timer::time_point tick) -> bool
{
    auto* PChar   = bot.entity();
    auto* PVictim = PTarget->GetBattleTarget();

    // A link or add on someone else comes first: the main tank grabs it, and an off-tank does when the main tank's Provoke is down.
    if (PChar->PParty != nullptr)
    {
        auto* PTank = mainTank(PChar);
        if (auto* PStray = strayMonster(PChar); PStray != nullptr && (PTank == PChar || !provokeReady(PTank)) && grab(bot, packets, PStray, tick))
        {
            return true;
        }
    }

    // The main tank builds a lead with every enmity tool as it comes off cooldown, whoever has the monster right now.
    if (mainTank(PChar) == PChar)
    {
        if (tryAbility(bot, packets, ABILITY_PROVOKE, PTarget, tick))
        {
            if (PVictim != nullptr && PVictim != PChar)
            {
                callout(bot, fmt::format("Provoke >> {}", pretty(PTarget->getName())), true);
            }
            return true;
        }
        return tryCast(bot, packets, SpellID::Flash, PTarget, std::chrono::seconds(5), tick) ||
               tryCast(bot, packets, SpellID::Foil, PChar, std::chrono::seconds(10), tick) ||
               (PChar->GetHPP() < 50 && tryAbility(bot, packets, ABILITY_SENTINEL, PChar, tick)) ||
               tryAbility(bot, packets, ABILITY_WARCRY, PChar, tick) ||
               (PVictim != PChar && tryAbility(bot, packets, ABILITY_SHIELD_BASH, PTarget, tick));
    }

    if (PVictim == nullptr || PVictim == PChar || (PVictim->PParty != PChar->PParty || PChar->PParty == nullptr))
    {
        return false;
    }

    // Off-tanks leave a tank with hate alone until it gets low.
    if (isTank(PVictim->GetMJob()) && PVictim->GetHPP() >= kTankHandOverHPP)
    {
        return false;
    }

    if (tryAbility(bot, packets, ABILITY_PROVOKE, PTarget, tick))
    {
        callout(bot, fmt::format("Provoke >> {}", pretty(PTarget->getName())), true);
        return true;
    }
    return tryCast(bot, packets, SpellID::Flash, PTarget, std::chrono::seconds(5), tick) ||
           tryAbility(bot, packets, ABILITY_SHIELD_BASH, PTarget, tick);
}

} // namespace

// The best tank in the party, players included: PLD, then NIN or RUN, then WAR; the higher level breaks a tie.
auto mainTank(CCharEntity* PChar) -> CBattleEntity*
{
    if (PChar->PParty == nullptr)
    {
        return nullptr;
    }

    const auto rank = [](const Job job)
    {
        switch (job)
        {
            case Job::PLD: return 3;
            case Job::NIN:
            case Job::RUN: return 2;
            case Job::WAR: return 1;
            default: return 0;
        }
    };

    CBattleEntity* PBest = nullptr;
    for (auto* PMember : PChar->PParty->members)
    {
        if (PMember->isDead() || PMember->getZone() != PChar->getZone() || rank(PMember->GetMJob()) == 0)
        {
            continue;
        }
        if (PBest == nullptr || rank(PMember->GetMJob()) > rank(PBest->GetMJob()) ||
            (rank(PMember->GetMJob()) == rank(PBest->GetMJob()) && PMember->GetMLevel() > PBest->GetMLevel()))
        {
            PBest = PMember;
        }
    }
    return PBest;
}

// Whom the party leans on for cures: its White Mage, else a Red Mage or Scholar; the higher level breaks a tie.
auto partyHealer(CCharEntity* PChar) -> CBattleEntity*
{
    if (PChar->PParty == nullptr)
    {
        return nullptr;
    }

    const auto rank = [](const Job job) { return job == Job::WHM ? 2 : job == Job::RDM || job == Job::SCH ? 1 : 0; };

    CBattleEntity* PBest = nullptr;
    for (auto* PMember : PChar->PParty->members)
    {
        if (PMember->isDead() || PMember->getZone() != PChar->getZone() || rank(PMember->GetMJob()) == 0)
        {
            continue;
        }
        if (PBest == nullptr || rank(PMember->GetMJob()) > rank(PBest->GetMJob()) ||
            (rank(PMember->GetMJob()) == rank(PBest->GetMJob()) && PMember->GetMLevel() > PBest->GetMLevel()))
        {
            PBest = PMember;
        }
    }
    return PBest;
}

// For !bot: this bot's hate on its target against whoever holds it.
auto hateStatus(CCharEntity* PChar, CBattleEntity* PTarget) -> std::string
{
    if (PTarget == nullptr)
    {
        return "Hate: not fighting";
    }
    auto*      PHolder = PTarget->GetBattleTarget();
    auto*      PTank   = mainTank(PChar);
    const auto mine    = enmityOn(PTarget, PChar);
    const auto theirs  = PHolder != nullptr ? enmityOn(PTarget, PHolder) : 0;
    const auto state   = PHolder == PChar ? "it's on me" : nearHate(PChar, PTarget) ? "holding back, close to pulling it" : "clear";
    return fmt::format("Hate on {}: mine {}, {} {}; {}; main tank {}", pretty(PTarget->getName()), mine, PHolder ? PHolder->getName() : std::string("nobody"),
                       theirs, state, PTank ? PTank->getName() : std::string("none"));
}

// Someone this healer should get up for: within range and below its cure threshold, dead with a raise to hand, or (for a
// White Mage) carrying an ailment it lifts. Only with the MP for a Cure.
auto healingWanted(CCharEntity* PChar) -> bool
{
    const auto  job    = PChar->GetMJob();
    const auto* PCure  = spell::GetSpell(SpellID::Cure);
    if (PCure == nullptr || !spell::CanUseSpell(PChar, SpellID::Cure) || PChar->health.mp < PCure->getMPCost())
    {
        return false;
    }

    const uint8 threshold = job == Job::WHM ? kWhmCureHPP : job == Job::RDM || job == Job::SCH ? 60 : kEmergencyHPP;
    const bool  raises    = spell::CanUseSpell(PChar, SpellID::Raise) || spell::CanUseSpell(PChar, SpellID::Raise_II);
    return std::ranges::any_of(partyMembersAll(PChar),
                               [&](CBattleEntity* PMember)
                               {
                                   if (distance(PChar->loc.p, PMember->loc.p) > kSpellRange)
                                   {
                                       return false;
                                   }
                                   if (PMember->isDead())
                                   {
                                       return raises;
                                   }
                                   return PMember->GetHPP() < threshold ||
                                          (job == Job::WHM &&
                                           std::ranges::any_of(kCleanses, [&](const Cleanse& c) { return PMember->StatusEffectContainer->HasStatusEffect(c.effect); }));
                               });
}

// A monster other than this bot's target is hitting it while a fit main tank could take it.
auto chasedOffTank(CCharEntity* PChar) -> bool
{
    auto* PTank = mainTank(PChar);
    if (PTank == nullptr || PTank == PChar || PTank->GetHPP() < kTankHandOverHPP)
    {
        return false;
    }

    bool chased = false;
    PChar->loc.zone->ForEachMob(
        [&](CMobEntity* PMob)
        {
            chased = chased || (PMob->isAlive() && PMob->PAI->IsEngaged() && PMob->GetBattleTarget() == PChar && !beingPulled(PChar, PMob) && !asleep(PMob));
        });
    return chased;
}

// Someone other than the main tank has the monster, and the tank is fit to take it back.
auto pulledHate(CCharEntity* PChar, CBattleEntity* PTarget) -> bool
{
    auto* PTank = mainTank(PChar);
    return PTarget->GetBattleTarget() == PChar && PTank != nullptr && PTank != PChar && PTank->GetHPP() >= kTankHandOverHPP;
}

namespace
{

// Dancer TP: a samba up, and a step for Finishing Moves when there are none; Reverse Flourish turns spare ones back into TP.
auto dance(Bot& bot, PacketSystem& packets, CBattleEntity* PTarget, const timer::time_point tick) -> bool
{
    auto*      PChar = bot.entity();
    const auto tp    = PChar->health.tp;
    if (!PChar->PAI->IsEngaged())
    {
        return false;
    }

    if (!PChar->StatusEffectContainer->HasStatusEffect({ StatusEffect::DrainSamba, StatusEffect::AspirSamba, StatusEffect::HasteSamba }) &&
        ((tp >= 350 && tryAbility(bot, packets, ABILITY_HASTE_SAMBA, PChar, tick)) || (tp >= 250 && tryAbility(bot, packets, ABILITY_DRAIN_SAMBA_II, PChar, tick)) ||
         (tp >= 100 && tryAbility(bot, packets, ABILITY_DRAIN_SAMBA, PChar, tick))))
    {
        return true;
    }

    const auto* PMoves = PChar->StatusEffectContainer->GetStatusEffect(StatusEffect::FinishingMove1);
    if (PMoves == nullptr)
    {
        return tp >= 100 && tp < 1000 && (tryAbility(bot, packets, ABILITY_BOX_STEP, PTarget, tick) || tryAbility(bot, packets, ABILITY_QUICKSTEP, PTarget, tick));
    }
    return PMoves->GetPower() >= 3 && tryAbility(bot, packets, ABILITY_REVERSE_FLOURISH, PChar, tick);
}

// Waltzes for whoever needs them most: a Dancer heals the party, anyone else with a waltz only in an emergency.
auto waltz(Bot& bot, PacketSystem& packets, const std::vector<CBattleEntity*>& members, const timer::time_point tick) -> bool
{
    auto*       PChar     = bot.entity();
    const uint8 threshold = PChar->GetMJob() == Job::DNC ? 55 : kEmergencyHPP;

    CBattleEntity* PHurt = nullptr;
    for (auto* PMember : members)
    {
        if (PMember->GetHPP() < threshold && distance(PChar->loc.p, PMember->loc.p) <= 18.0f && (PHurt == nullptr || PMember->GetHPP() < PHurt->GetHPP()))
        {
            PHurt = PMember;
        }
    }
    if (PHurt == nullptr)
    {
        return false;
    }

    static constexpr std::array<std::pair<uint16, int16>, 5> waltzes = { { { ABILITY_CURING_WALTZ_V, 800 },
                                                                           { ABILITY_CURING_WALTZ_IV, 650 },
                                                                           { ABILITY_CURING_WALTZ_III, 500 },
                                                                           { ABILITY_CURING_WALTZ_II, 350 },
                                                                           { ABILITY_CURING_WALTZ, 200 } } };
    return std::ranges::any_of(waltzes, [&](const auto& w) { return PChar->health.tp >= w.second && tryAbility(bot, packets, w.first, PHurt, tick); });
}

// Rune Fencer: as many runes as it can hold, Swordplay, and Vallation, Valiance or Pflug while it takes the hits.
auto runes(Bot& bot, PacketSystem& packets, CBattleEntity* PTarget, const timer::time_point tick) -> bool
{
    auto* PChar = bot.entity();
    if (PChar->GetMJob() != Job::RUN)
    {
        return false;
    }

    const auto held = std::ranges::count_if(std::array{ StatusEffect::Ignis, StatusEffect::Gelus, StatusEffect::Flabra, StatusEffect::Tellus, StatusEffect::Sulpor,
                                                        StatusEffect::Unda, StatusEffect::Lux, StatusEffect::Tenebrae },
                                            [&](const StatusEffect effect) { return PChar->StatusEffectContainer->HasStatusEffect(effect); });
    const auto most = PChar->GetMLevel() >= 65 ? 3 : PChar->GetMLevel() >= 35 ? 2 : 1;
    const auto self = [&](const uint16 id) { return tryAbility(bot, packets, id, PChar, tick); };
    if (held < most && self(ABILITY_TELLUS))
    {
        return true;
    }
    if (held == 0)
    {
        return false;
    }

    const bool hit = PTarget->GetBattleTarget() == PChar;
    return (PChar->PAI->IsEngaged() && !PChar->StatusEffectContainer->HasStatusEffect(StatusEffect::Swordplay) && self(ABILITY_SWORDPLAY)) ||
           (hit && !PChar->StatusEffectContainer->HasStatusEffect({ StatusEffect::Vallation, StatusEffect::Valiance }) &&
            (self(ABILITY_VALIANCE) || self(ABILITY_VALLATION))) ||
           (hit && PChar->GetHPP() < 70 && !PChar->StatusEffectContainer->HasStatusEffect(StatusEffect::Pflug) && self(ABILITY_PFLUG));
}

// A monster mid-spell is bashed or stunned out of it, by whoever can.
auto interrupt(Bot& bot, PacketSystem& packets, CBattleEntity* PTarget, const timer::time_point tick) -> bool
{
    auto* PChar = bot.entity();
    if (!PTarget->PAI->IsCurrentState<CMagicState>())
    {
        return false;
    }

    const auto* PWeapon = dynamic_cast<CItemWeapon*>(PChar->m_Weapons[SLOT_MAIN]);
    const bool  melee   = PChar->PAI->IsEngaged() && distance(PChar->loc.p, PTarget->loc.p) <= 5.0f;
    if ((melee && (tryAbility(bot, packets, ABILITY_SHIELD_BASH, PTarget, tick) ||
                   (PWeapon != nullptr && PWeapon->isTwoHanded() && tryAbility(bot, packets, ABILITY_WEAPON_BASH, PTarget, tick)) ||
                   (PChar->StatusEffectContainer->HasStatusEffect(StatusEffect::FinishingMove1) && tryAbility(bot, packets, ABILITY_VIOLENT_FLOURISH, PTarget, tick)))) ||
        tryCast(bot, packets, SpellID::Stun, PTarget, std::chrono::seconds(3), tick) || tryCast(bot, packets, SpellID::Head_Butt, PTarget, std::chrono::seconds(3), tick))
    {
        callout(bot, fmt::format("Interrupting {}!", pretty(PTarget->getName())), true);
        return true;
    }
    return false;
}

// Job abilities from the main job and the subjob alike. Damage buffs are for the damage dealers, and wait while hate is close.
auto jobAbilities(Bot& bot, PacketSystem& packets, CBattleEntity* PTarget, const timer::time_point tick) -> bool
{
    auto* PChar = bot.entity();

    if ((isTank(PChar->GetMJob()) || isTank(PChar->GetSJob())) && tank(bot, packets, PTarget, tick))
    {
        return true;
    }

    auto*      PTank    = mainTank(PChar);
    const bool tanking  = PTank == PChar;
    const bool targeted = PTarget->GetBattleTarget() == PChar;
    const bool hold     = holdDamage(PChar, PTarget);
    const bool healer   = partyHas(PChar, Job::WHM) || partyHas(PChar, Job::RDM) || partyHas(PChar, Job::SCH);
    const auto self     = [&](const uint16 id) { return tryAbility(bot, packets, id, PChar, tick); };

    // Staying alive.
    if ((PChar->GetHPP() < 50 && (self(ABILITY_CHAKRA) || self(ABILITY_SENTINEL))) || (tanking && PChar->GetHPP() < 60 && self(ABILITY_DEFENDER)) ||
        (targeted && (self(ABILITY_THIRD_EYE) || self(ABILITY_DODGE))))
    {
        return true;
    }

    // Ninjas tank in Yonin and deal damage in Innin.
    if (PChar->GetMJob() == Job::NIN && !PChar->StatusEffectContainer->HasStatusEffect({ StatusEffect::Yonin, StatusEffect::Innin }) &&
        self(tanking ? ABILITY_YONIN : ABILITY_INNIN))
    {
        return true;
    }

    // Meditate before the first weapon skill is ready.
    if (PChar->health.tp < 1000 && PChar->PAI->IsEngaged() && self(ABILITY_MEDITATE))
    {
        return true;
    }

    // A circle against the kind of monster it wards.
    for (const auto& [ecosystem, ability, effect] : { std::tuple{ xi::Ecosystem::Undead, ABILITY_HOLY_CIRCLE, StatusEffect::HolyCircle },
                                                      std::tuple{ xi::Ecosystem::Arcana, ABILITY_ARCANE_CIRCLE, StatusEffect::ArcaneCircle },
                                                      std::tuple{ xi::Ecosystem::Demon, ABILITY_WARDING_CIRCLE, StatusEffect::WardingCircle },
                                                      std::tuple{ xi::Ecosystem::Dragon, ABILITY_ANCIENT_CIRCLE, StatusEffect::AncientCircle } })
    {
        if (PTarget->m_EcoSystem == ecosystem && !PChar->StatusEffectContainer->HasStatusEffect(effect) && self(ability))
        {
            return true;
        }
    }

    if (runes(bot, packets, PTarget, tick))
    {
        return true;
    }

    if (!tanking && !hold)
    {
        if (self(ABILITY_BERSERK) || self(ABILITY_AGGRESSOR) || self(ABILITY_WARCRY) || self(ABILITY_LAST_RESORT) ||
            (!PChar->StatusEffectContainer->HasStatusEffect(StatusEffect::Seigan) && self(ABILITY_HASSO)) || self(ABILITY_FOCUS) ||
            (PChar->GetHPP() > 75 && healer && self(ABILITY_SOULEATER)) || self(ABILITY_SHARPSHOT) || self(ABILITY_BARRAGE))
        {
            return true;
        }
        if ((PTank != nullptr && self(ABILITY_TRICK_ATTACK)) || self(ABILITY_SNEAK_ATTACK) || tryAbility(bot, packets, ABILITY_MUG, PTarget, tick) ||
            tryAbility(bot, packets, ABILITY_CHI_BLAST, PTarget, tick))
        {
            return true;
        }
        if (dance(bot, packets, PTarget, tick) || (PChar->health.tp >= 2000 && self(ABILITY_SEKKANOKI)) || (PChar->health.tp < 1000 && self(ABILITY_BOOST)))
        {
            return true;
        }
    }

    // High Jump sheds enmity, so it's the one to use when hate is close.
    return tryAbility(bot, packets, ABILITY_HIGH_JUMP, PTarget, tick) || (!hold && tryAbility(bot, packets, ABILITY_JUMP, PTarget, tick));
}

// Players and bots close by who aren't in this bot's party.
auto strangers(CCharEntity* PChar, const float range) -> std::vector<CCharEntity*>
{
    std::vector<CCharEntity*> found;
    if (PChar->loc.zone == nullptr)
    {
        return found;
    }

    PChar->loc.zone->ForEachChar(
        [&](CCharEntity* POther)
        {
            if (POther != PChar && (PChar->PParty == nullptr || POther->PParty != PChar->PParty) && POther->status != xi::Status::Disappear &&
                POther->m_moghouseID == 0 && distance(PChar->loc.p, POther->loc.p) <= range)
            {
                found.push_back(POther);
            }
        });
    return found;
}

auto canRaise(CBattleEntity* PCaster) -> bool
{
    return !PCaster->isDead() && (spell::CanUseSpell(PCaster, SpellID::Raise) || spell::CanUseSpell(PCaster, SpellID::Raise_II));
}

// Kindness to people passing by: raise the dead, cure the badly hurt, and now and then a Protect or Shell outside town.
auto helpStrangers(Bot& bot, PacketSystem& packets, const timer::time_point tick) -> bool
{
    auto*      PChar  = bot.entity();
    const auto nearby = strangers(PChar, kSpellRange);

    for (auto* POther : nearby)
    {
        // Someone who turned a raise down isn't offered another for a while.
        auto& again = bot.attempts[(static_cast<uint64>(POther->id) << 32) | kRaisedKey];
        if (POther->isDead() && !POther->m_hasRaise && tick >= again &&
            castBest(bot, packets, { SpellID::Raise_II, SpellID::Raise }, POther, std::chrono::seconds(15), tick))
        {
            again = tick + std::chrono::minutes(2);
            static constexpr std::array<const char*, 4> lines = { "raise incoming", "hang on, raising you", "rr on the way", "got you, raise coming" };
            if (!isPoolChar(POther->id) && throttle(bot, kTellKey, POther->id, std::chrono::minutes(2), tick))
            {
                tell(bot, POther->getName(), lines[xirand::GetRandomNumber<std::size_t>(lines.size())]);
            }
            return true;
        }
    }

    std::vector<CBattleEntity*> living;
    for (auto* POther : nearby)
    {
        if (!POther->isDead())
        {
            living.push_back(POther);
        }
    }
    // The party's MP comes first: passers-by get what is spare.
    if (PChar->GetMPP() >= kStrangerCureMPP && heal(bot, packets, living, 50, tick))
    {
        return true;
    }

    const bool town = (static_cast<uint16>(PChar->loc.zone->GetTypeMask()) & static_cast<uint16>(xi::ZoneType::City)) != 0;
    if (town || PChar->GetMPP() < kStrangerBuffMPP)
    {
        return false;
    }
    for (auto* POther : living)
    {
        for (const auto& [effect, ladder] : { std::pair{ StatusEffect::Protect, std::array{ SpellID::Protect_III, SpellID::Protect_II, SpellID::Protect } },
                                              std::pair{ StatusEffect::Shell, std::array{ SpellID::Shell_III, SpellID::Shell_II, SpellID::Shell } } })
        {
            if (!POther->StatusEffectContainer->HasStatusEffect(effect) && throttle(bot, kKindKey | static_cast<uint32>(effect), POther->id, std::chrono::minutes(10), tick) &&
                std::ranges::any_of(ladder, [&](const SpellID id) { return tryCast(bot, packets, id, POther, std::chrono::seconds(30), tick); }))
            {
                return true;
            }
        }
    }
    return false;
}

} // namespace

// A dead player or bot a free healer should walk over to and raise.
auto deadToRaise(CCharEntity* PChar) -> CCharEntity*
{
    if (!canRaise(PChar))
    {
        return nullptr;
    }
    for (auto* POther : strangers(PChar, kNoticeRange))
    {
        if (POther->isDead() && !POther->m_hasRaise)
        {
            return POther;
        }
    }
    return nullptr;
}

// How far the puller can open from: a ranged attack or Dia from casting range, Provoke a little closer; 0 for melee only.
auto pullRange(CCharEntity* PChar) -> float
{
    if (isRanged(PChar) || spell::CanUseSpell(PChar, SpellID::Dia) || spell::CanUseSpell(PChar, SpellID::Battlefield_Elegy) ||
        spell::CanUseSpell(PChar, SpellID::Foe_Requiem))
    {
        return 18.0f;
    }
    return charutils::hasAbility(PChar, ABILITY_PROVOKE) ? 14.0f : 0.0f;
}

// Bards pull with Elegy: it lands from range and slows the monster on the way in; Requiem before it's learnt.
auto openPull(Bot& bot, PacketSystem& packets, CBattleEntity* PMob, const timer::time_point tick) -> bool
{
    return castBest(bot, packets, { SpellID::Carnage_Elegy, SpellID::Battlefield_Elegy, SpellID::Foe_Requiem_III, SpellID::Foe_Requiem_II, SpellID::Foe_Requiem },
                    PMob, std::chrono::seconds(5), tick) ||
           shoot(bot, packets, PMob, tick) || tryAbility(bot, packets, ABILITY_PROVOKE, PMob, tick) ||
           castBest(bot, packets, { SpellID::Dia_II, SpellID::Dia }, PMob, std::chrono::seconds(5), tick);
}

// A link on the way back goes to sleep and the puller keeps running; Foe Lullaby when it's next to the pulled one.
auto sleepLink(Bot& bot, PacketSystem& packets, CBattleEntity* PMob, const CBattleEntity* PBesides, const timer::time_point tick) -> bool
{
    return trySleep(bot, packets, PMob, PBesides != nullptr && distance(PMob->loc.p, PBesides->loc.p) < kHordeRadius ? SleepKind::Single : SleepKind::Area, tick);
}

// Chocobo Mazurka for the run out.
auto speedUp(Bot& bot, PacketSystem& packets, const timer::time_point tick) -> bool
{
    auto* PChar = bot.entity();
    return !PChar->StatusEffectContainer->HasStatusEffect(StatusEffect::Mazurka) &&
           tryCast(bot, packets, SpellID::Chocobo_Mazurka, PChar, std::chrono::seconds(30), tick);
}

// Worth standing up for: an add to sleep and the MP for the cheapest sleep this bot knows.
auto addNeedsSleep(CCharEntity* PChar, CBattleEntity* PTarget) -> bool
{
    if (findAdd(PChar, PTarget) == nullptr)
    {
        return false;
    }
    return std::ranges::any_of(std::array{ SpellID::Foe_Lullaby, SpellID::Horde_Lullaby, SpellID::Sleep, SpellID::Sleep_II, SpellID::Sleepga },
                               [&](const SpellID id)
                               {
                                   const auto* PSpell = spell::GetSpell(id);
                                   return PSpell != nullptr && spell::CanUseSpell(PChar, id) && PChar->health.mp >= PSpell->getMPCost();
                               });
}

// Sneak and Invisible to match whoever the bot follows: the spell on itself if it knows it, else Silent Oil or Prism Powder.
auto conceal(Bot& bot, PacketSystem& packets, const CCharEntity* PGuide, const timer::time_point tick) -> bool
{
    auto* PChar = bot.entity();
    if (PGuide->getZone() != PChar->getZone() || PChar->PAI->IsEngaged())
    {
        return false;
    }

    for (const auto& [effect, spell, item] : { std::tuple{ StatusEffect::Sneak, SpellID::Sneak, kSilentOil }, std::tuple{ StatusEffect::Invisible, SpellID::Invisible, kPrismPowder } })
    {
        if (!PGuide->StatusEffectContainer->HasStatusEffect(effect) || PChar->StatusEffectContainer->HasStatusEffect(effect))
        {
            continue;
        }
        if (tryCast(bot, packets, spell, PChar, std::chrono::seconds(10), tick) || tryAbility(bot, packets, ABILITY_SPECTRAL_JIG, PChar, tick) ||
            (throttle(bot, kItemKey | item, PChar->id, std::chrono::seconds(10), tick) && useItem(bot, packets, item, PChar)))
        {
            return true;
        }
    }
    return false;
}

auto asleep(const CBattleEntity* PEntity) -> bool
{
    return PEntity->StatusEffectContainer->HasStatusEffect({ StatusEffect::SleepI, StatusEffect::SleepIi, StatusEffect::Lullaby });
}

auto prettyName(const std::string& name) -> std::string
{
    return pretty(name);
}

auto raiserNearby(CCharEntity* PChar) -> bool
{
    return std::ranges::any_of(strangers(PChar, kNoticeRange), [](CCharEntity* POther) { return canRaise(POther); });
}

auto throttle(Bot& bot, const uint32 key, const uint32 targetId, const timer::duration cooldown, const timer::time_point tick) -> bool
{
    auto& next = bot.attempts[(static_cast<uint64>(targetId) << 32) | key];
    if (tick < next)
    {
        return false;
    }

    next = tick + cooldown;
    return true;
}

auto isCaster(const Job job) -> bool
{
    switch (job)
    {
        case Job::WHM:
        case Job::BLM:
        case Job::BRD:
        case Job::SMN:
        case Job::SCH:
        case Job::GEO:
            return true;
        default:
            return false;
    }
}

// A ranger with a bow or gun and something to fire from it.
auto isRanged(CCharEntity* PChar) -> bool
{
    if (PChar->GetMJob() != Job::RNG && PChar->GetMJob() != Job::COR)
    {
        return false;
    }

    const auto* PRanged = dynamic_cast<CItemWeapon*>(PChar->getEquip(SLOT_RANGED));
    return PRanged != nullptr && PChar->getEquip(SLOT_AMMO) != nullptr &&
           (PRanged->getSkillType() == xi::SkillType::Archery || PRanged->getSkillType() == xi::SkillType::Marksmanship);
}

auto canAct(CCharEntity* PChar) -> bool
{
    auto* PController = static_cast<CPlayerController*>(PChar->PAI->GetController());
    return PChar->PAI->CanChangeState() && PController->canAct() && !PChar->StatusEffectContainer->HasPreventActionEffect();
}

auto act(Bot& bot, PacketSystem& packets, CBattleEntity* PTarget, const bool travelling, const timer::time_point tick) -> bool
{
    auto* PChar = bot.entity();
    if (!canAct(PChar))
    {
        return false;
    }
    learnNuke(bot, PChar, tick);

    const auto members = partyMembers(PChar);
    const auto job     = PChar->GetMJob();

    // On the move out of a fight, nothing but an emergency cure.
    if (travelling && PTarget == nullptr)
    {
        switch (job)
        {
            case Job::WHM:
            case Job::RDM:
            case Job::SCH:
            case Job::PLD:
                return heal(bot, packets, members, kEmergencyHPP, tick);
            default:
                return false;
        }
    }

    if (emergency(bot, packets, members, PTarget, tick))
    {
        return true;
    }

    if (PChar->GetMaxMP() > 0 && PChar->StatusEffectContainer->HasStatusEffect(StatusEffect::Silence) &&
        throttle(bot, kItemKey | kEchoDrops, PChar->id, std::chrono::seconds(5), tick) && useItem(bot, packets, kEchoDrops, PChar))
    {
        return true;
    }

    // Healing comes first for every job that can, in or out of a fight.
    switch (job)
    {
        case Job::WHM:
            if (heal(bot, packets, members, kWhmCureHPP, tick))
            {
                return true;
            }
            break;
        case Job::RDM:
        case Job::SCH:
            if (heal(bot, packets, members, 60, tick))
            {
                return true;
            }
            break;
        case Job::PLD:
            if (heal(bot, packets, members, 40, tick))
            {
                return true;
            }
            break;
        case Job::BLU:
            for (auto* PMember : members)
            {
                if (PMember == PChar ? PChar->GetHPP() < 50 : PMember->GetHPP() < kEmergencyHPP)
                {
                    const bool cured = PMember == PChar ? castBest(bot, packets, { SpellID::Magic_Fruit, SpellID::Wild_Carrot, SpellID::Healing_Breeze, SpellID::Pollen }, PChar,
                                                                   std::chrono::seconds(4), tick)
                                                        : castBest(bot, packets, { SpellID::Magic_Fruit, SpellID::Wild_Carrot }, PMember, std::chrono::seconds(4), tick);
                    if (cured)
                    {
                        return true;
                    }
                }
            }
            break;
        default:
            if ((PChar->GetSJob() == Job::WHM || PChar->GetSJob() == Job::RDM) && heal(bot, packets, members, kEmergencyHPP, tick))
            {
                return true;
            }
            break;
    }

    if (waltz(bot, packets, members, tick))
    {
        return true;
    }

    if ((job == Job::WHM || job == Job::RDM || job == Job::SCH || job == Job::PLD) && cleanse(bot, packets, members, job == Job::WHM, tick))
    {
        return true;
    }

    // The dead first, in or out of a fight.
    if (PChar->PParty != nullptr)
    {
        for (auto* PMember : PChar->PParty->members)
        {
            if (PMember != PChar && PMember->isDead() && PMember->getZone() == PChar->getZone() &&
                castBest(bot, packets, { SpellID::Raise_II, SpellID::Raise }, PMember, std::chrono::seconds(15), tick))
            {
                callout(bot, fmt::format("Raise >> {}", PMember->getName()), false);
                return true;
            }
        }
    }

    if (PTarget != nullptr && sleepAdds(bot, packets, PTarget, tick))
    {
        return true;
    }

    if (job == Job::BRD && sing(bot, packets, members, tick))
    {
        return true;
    }

    if (upkeep(bot, packets, members, PTarget, tick))
    {
        return true;
    }

    if (pets(bot, packets, PTarget, tick))
    {
        return true;
    }

    if (PTarget == nullptr)
    {
        // Out of combat: keep the party protected.
        if ((job == Job::WHM || job == Job::RDM || job == Job::SCH) && support(bot, packets, members, nullptr, tick))
        {
            return true;
        }

        // Then anyone else around who could use a hand.
        return (job == Job::WHM || job == Job::RDM || job == Job::SCH || job == Job::PLD) && helpStrangers(bot, packets, tick);
    }

    if (shadows(bot, packets, tick) || interrupt(bot, packets, PTarget, tick))
    {
        return true;
    }

    if (!isCaster(job) && jobAbilities(bot, packets, PTarget, tick))
    {
        return true;
    }

    if (!isCaster(job) && (PChar->PAI->IsEngaged() || isRanged(PChar)) && tryWeaponSkill(bot, packets, PTarget, tick))
    {
        return true;
    }

    if (support(bot, packets, members, PTarget, tick))
    {
        return true;
    }

    switch (job)
    {
        case Job::RDM:
            return debuff(bot, packets, PTarget, tick) || (PChar->GetMPP() > 50 && nuke(bot, packets, PTarget, std::chrono::seconds(15), tick));
        case Job::WHM:
            // Dia always; Slow, Paralyze and Silence only when there's no red mage to do it and MP to spare.
            if (enfeeble(bot, packets, PTarget, StatusEffect::Dia, { SpellID::Dia_II, SpellID::Dia }, tick))
            {
                return true;
            }
            if (partyHas(PChar, Job::RDM) || PChar->GetMPP() < 60)
            {
                return false;
            }
            return enfeeble(bot, packets, PTarget, StatusEffect::Slow, { SpellID::Slow }, tick) ||
                   enfeeble(bot, packets, PTarget, StatusEffect::Paralysis, { SpellID::Paralyze }, tick);
        case Job::BLM:
        case Job::SCH:
        case Job::GEO:
            return nuke(bot, packets, PTarget, std::chrono::seconds(6), tick);
        case Job::BLU:
        {
            // Defence when it's being hit, then physical blue magic while hate allows and MP lasts.
            if (PTarget->GetBattleTarget() == PChar &&
                ((!PChar->StatusEffectContainer->HasStatusEffect(StatusEffect::DefenseBoost) && tryCast(bot, packets, SpellID::Cocoon, PChar, std::chrono::seconds(10), tick)) ||
                 (!PChar->StatusEffectContainer->HasStatusEffect(StatusEffect::Stoneskin) && tryCast(bot, packets, SpellID::Metallic_Body, PChar, std::chrono::seconds(10), tick))))
            {
                return true;
            }
            if (holdDamage(PChar, PTarget) || PChar->GetMPP() < 25)
            {
                return false;
            }
            return castBest(bot, packets,
                            { SpellID::Disseverment, SpellID::Hysteric_Barrage, SpellID::Sickle_Slash, SpellID::Jet_Stream, SpellID::Death_Ray, SpellID::Screwdriver,
                              SpellID::Bludgeon, SpellID::Sound_Blast, SpellID::Power_Attack },
                            PTarget, std::chrono::seconds(8), tick);
        }
        case Job::DRK:
        {
            // Undead shrug off both.
            if (PTarget->m_EcoSystem == xi::Ecosystem::Undead)
            {
                return false;
            }
            const bool aspir = PChar->GetMPP() < 50 && PTarget->health.mp > 0;
            return (aspir && tryCast(bot, packets, SpellID::Aspir, PTarget, std::chrono::seconds(10), tick)) ||
                   (PChar->GetHPP() < 70 && !holdDamage(PChar, PTarget) && tryCast(bot, packets, SpellID::Drain, PTarget, std::chrono::seconds(10), tick));
        }
        default:
            return shoot(bot, packets, PTarget, tick);
    }
}

// Whom to follow or assist, as kept on the bot: 0 stands for the party leader (and for itself).
auto partnerId(CCharEntity* PChar, const uint32 id) -> uint32
{
    if (PChar->PParty == nullptr || id == PChar->id || PChar->PParty->GetLeader() == nullptr || PChar->PParty->GetLeader()->id == id)
    {
        return 0;
    }
    return id;
}

// Carries out a request from chat. True while it holds the bot's attention, so nothing else casts over it.
auto followOrder(Bot& bot, PacketSystem& packets, const timer::time_point tick) -> bool
{
    if (!bot.order && !bot.queued.empty())
    {
        bot.order = bot.queued.front();
        bot.order->expires = tick + std::chrono::seconds(20);
        bot.queued.pop_front();
    }
    if (!bot.order)
    {
        return false;
    }

    auto& order = *bot.order;
    auto* PChar = bot.entity();
    if (tick > order.expires)
    {
        bot.order.reset();
        return false;
    }

    // "That monster" is whatever the person asking is fighting.
    CBattleEntity* PMonster = nullptr;
    if (auto* PSpeaker = zoneutils::GetChar(order.speakerId); PSpeaker != nullptr && PSpeaker->getZone() == PChar->getZone())
    {
        PMonster = PSpeaker->GetBattleTarget();
        if (PMonster != nullptr && (PMonster->objtype != TYPE_MOB || PMonster->isDead()))
        {
            PMonster = nullptr;
        }
    }

    switch (order.kind)
    {
        case OrderKind::Cast:
        {
            if (!canAct(PChar))
            {
                return true;
            }

            const auto castAny = [&](CBattleEntity* PTarget)
            {
                return std::ranges::any_of(order.spells,
                                           [&](const SpellID id)
                                           {
                                               return tryCast(bot, packets, id, PTarget, 0s, tick);
                                           });
            };

            if (order.targets.empty())
            {
                if (PMonster != nullptr && castAny(PMonster))
                {
                    bot.order.reset();
                    return true;
                }
                return false;
            }

            for (auto it = order.targets.begin(); it != order.targets.end();)
            {
                auto* PTarget = zoneutils::GetChar(*it);
                if (PTarget == nullptr || PTarget->getZone() != PChar->getZone())
                {
                    it = order.targets.erase(it);
                    continue;
                }

                if (castAny(PTarget))
                {
                    order.targets.erase(it);
                    if (order.targets.empty())
                    {
                        bot.order.reset();
                    }
                    return true;
                }
                ++it;
            }

            if (order.targets.empty())
            {
                bot.order.reset();
            }
            return false;
        }
        case OrderKind::Provoke:
        {
            if (PMonster == nullptr)
            {
                return false;
            }
            if (!canAct(PChar))
            {
                return true;
            }
            if (tryAbility(bot, packets, ABILITY_PROVOKE, PMonster, tick) || tryCast(bot, packets, SpellID::Flash, PMonster, 0s, tick))
            {
                bot.order.reset();
                return true;
            }
            return false;
        }
        case OrderKind::Attack:
            if (PMonster != nullptr)
            {
                bot.focusTarget = PMonster->id;
                bot.holdTarget  = 0;
            }
            break;
        case OrderKind::Stop:
            bot.holdTarget  = PMonster ? PMonster->id : (PChar->GetBattleTarget() ? PChar->GetBattleTarget()->id : 0);
            bot.focusTarget = 0;
            break;
        case OrderKind::Rest:
            bot.restNow = true;
            break;
        case OrderKind::Stay:
            bot.stay = true;
            break;
        case OrderKind::Follow:
            bot.stay       = false;
            bot.holdTarget = 0;
            bot.followId   = partnerId(PChar, order.subject != 0 ? order.subject : order.speakerId);
            if (PChar->PParty != nullptr)
            {
                breakCamp(PChar->PParty->GetLeader());
            }
            break;
        case OrderKind::Assist:
            bot.holdTarget = 0;
            bot.assistId   = partnerId(PChar, order.subject != 0 ? order.subject : order.speakerId);
            break;
        case OrderKind::Pull:
            if (PChar->PParty != nullptr)
            {
                setPuller(PChar->PParty->GetLeader(), PChar->id);
            }
            break;
        case OrderKind::StopPull:
            if (PChar->PParty != nullptr)
            {
                stopPulling(PChar->PParty->GetLeader());
            }
            break;
        case OrderKind::Camp:
            if (PChar->PParty != nullptr)
            {
                setCamp(PChar->PParty->GetLeader());
            }
            break;
    }

    bot.order.reset();
    return false;
}

} // namespace bots
