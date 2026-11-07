//
// Pulling: one bot brings monsters back to camp.
//
//   Camp is where the leader said "camp here", or stood when the first puller was named. Naming a puller again
//   keeps the camp. While the leader is away from it, pulling pauses.
//
//   The puller picks the nearest suitable monster within reach of camp and goes once the party will be ready by the
//   time it gets back with it (HP, MP, the fight at camp over, the tank within a Cure of full), counting the resting,
//   Refresh and cures in between. It goes straight out, opens on it and runs back. Whenever it holds off it says why,
//   once. The leader's "pull anyway" skips the waits.
//

#include "bots.h"

#include "common/logging.h"
#include "common/settings.h"

#include "map/ai/ai_container.h"
#include "map/ai/helpers/pathfind.h"
#include "map/entities/char_entity.h"
#include "map/entities/mob_entity.h"
#include "map/party.h"
#include "map/spell.h"
#include "map/status_effect.h"
#include "map/status_effect_container.h"
#include "map/utils/zoneutils.h"
#include "map/zone.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <vector>

namespace bots
{

namespace
{

enum class Stage : uint8
{
    Waiting,   // at camp until the party is ready
    Outbound,  // going out to the chosen monster and opening on it
    Returning, // bringing it back
};

struct Pull
{
    uint32                pullerId{};
    xi::ZoneId            zone{};
    position_t            camp{};
    Stage                 stage{};
    EntityId              mob;
    timer::time_point     stageAt{};
    timer::time_point     deadline{};  // give up on this stage after
    bool                  announced{}; // "Pulling X" goes out once per pull
    EntityId              gaveUpOn;    // a monster it couldn't get, left alone for a while
    timer::time_point     gaveUpUntil{};
    timer::time_point     heldUntil{}; // called back: no new pull before this
    bool                  force{};     // the leader said "pull anyway": skip the waits and the usual picks
    std::set<std::string> waitSaid;    // why it's waiting, each said once until the next pull
    timer::time_point     leftAt{};    // left camp on this pull
    float                 expected{};  // seconds this pull should take, by distance
    float                 pace{ 1.0f }; // what this party's pulls really take against that, averaged
    EntityId              fightMob;    // the monster the party is on, to see how fast it's going down
    timer::time_point     fightSeenAt{};
    uint8                 fightSeenHPP{};
};

// By party leader.
std::map<uint32, Pull> pulls;

// What the party told the puller to go for or leave alone, by monster name as shown ("Siege Bat"). By party leader.
struct Choice
{
    xi::ZoneId               zone{};
    std::vector<std::string> only; // empty for anything
    std::vector<std::string> avoid;
};
std::map<uint32, Choice> choices;

constexpr auto kGiveUpFor  = std::chrono::minutes(2);
constexpr auto kRecallHold = std::chrono::seconds(20); // called back: back at camp before the next pull

constexpr float kLinkRadius  = 8.0f;  // other monsters this close to a target might link
constexpr int   kWeakestGap  = -3;    // levels below the party's highest, for the usual pick
constexpr int   kFallbackGap = -9;    // and when nothing else is around
constexpr float kPullNear    = 50.0f; // how far from camp the puller looks first
constexpr float kPullLimit   = 180.0f; // and the furthest it goes, when there's nothing closer
constexpr float kCampReached = 3.0f;
constexpr auto  kSpareTime   = std::chrono::seconds(30); // on top of the running time, for a trip's time limit
constexpr float kCampDrift   = 20.0f; // the leader this far from camp pauses pulling
constexpr uint8 kTroubleHPP  = 40;
constexpr uint8 kLeaveAtHPP  = 10; // the monster at camp this low: the next can be pulled
constexpr uint8 kTankPullHPP = 75; // the main tank's HP before another comes in on top of one
constexpr float kRunSpeed    = 5.0f; // yalms a second
constexpr float kTankGrab    = 15.0f; // the main tank takes the pulled monster once it's this close
constexpr float kPullerHome  = 8.0f;  // the puller this close to camp is back, and everyone joins in
constexpr float kOpenSeconds = 4.0f;  // opening on a monster and turning for home
constexpr float kSitSeconds  = 3.0f;  // a bot that will rest is down this soon
constexpr float kRegenEvery  = 3.0f;  // Regen and Refresh tick
constexpr float kWatchFight  = 6.0f;  // seconds of a fight before guessing when it ends
constexpr float kFightSpare  = 3.0f;  // the monster at camp dead this long before the next gets there

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

// Seconds from leaving camp until the monster is at camp, by distance: out to opening range, the opener, and back.
auto tripSeconds(CCharEntity* PChar, const Pull& pull, const CMobEntity* PMob) -> float
{
    const float range = pullRange(PChar);
    const float reach = range > 0.0f ? range : 3.0f;
    return 2.0f * std::max(0.0f, distance(pull.camp, PMob->loc.p) - reach) / kRunSpeed + kOpenSeconds;
}

// A bot gets down to /heal at the first lull if it's a caster missing anything, or below the rest line when resting
// costs it no TP (bot_ai.cpp rest()).
auto willRest(const CBattleEntity* PMember) -> bool
{
    if (!isPoolChar(PMember->id))
    {
        return false;
    }
    const bool full   = PMember->GetHPP() == 100 && (PMember->GetMaxMP() == 0 || PMember->GetMPP() == 100);
    const bool caster = (isCaster(PMember->GetMJob()) || PMember->GetMJob() == xi::Job::RDM) && PMember->PPet == nullptr;
    const bool lowHP  = PMember->GetHPP() < kRestHPP;
    const bool lowMP  = PMember->GetMaxMP() > 0 && PMember->GetMPP() < kRestMPP;
    return !full && (caster || (lowHP && (signetWorks(PMember) || PMember->health.tp == 0)) || (lowMP && PMember->health.tp == 0));
}

struct Outlook
{
    int32 hp{};
    int32 mp{};
};

// HP and MP a member should have in `seconds`, the first `fighting` of them still in the fight at camp: Regen and Refresh
// throughout, and /heal ticks (as scripts/effects/healing.lua gives them) for anyone resting or about to. The puller runs.
auto outlook(const CBattleEntity* PMember, const bool puller, const float fighting, const float seconds, const timer::time_point tick) -> Outlook
{
    Outlook    ahead{ PMember->health.hp, PMember->health.mp };
    const auto regenTicks = static_cast<int32>(seconds / kRegenEvery);
    ahead.hp += std::max(PMember->getMod(xi::Mod::REGEN) - PMember->getMod(xi::Mod::REGEN_DOWN), 0) * regenTicks;
    ahead.mp += std::max(PMember->getMod(xi::Mod::REFRESH) - PMember->getMod(xi::Mod::REFRESH_DOWN), 0) * regenTicks;

    // When it sat down, in seconds from now, and the ticks it has had.
    std::optional<float> sat;
    int                  ticked   = 0;
    const auto*          PResting = PMember->StatusEffectContainer->GetStatusEffect(xi::StatusEffect::Healing);
    if (PResting != nullptr && fighting <= 0.0f)
    {
        sat    = -std::chrono::duration<float>(tick - PResting->GetStartTime()).count();
        ticked = PResting->GetElapsedTickCount();
    }
    else if (!puller && willRest(PMember))
    {
        sat = fighting + kSitSeconds;
    }

    const float every  = std::max<float>(1.0f, settings::get<uint8>("map.HEALING_TICK_DELAY"));
    const bool  signet = signetWorks(PMember);
    for (int n = ticked + 1; sat && *sat + n * every <= seconds; ++n)
    {
        if (n < 2)
        {
            continue; // the first tick heals nothing
        }
        ahead.hp += (signet ? 10 + 3 * (PMember->GetMLevel() / 10) + (n - 2) * (1 + PMember->GetMaxHP() / 300) : 10 + (n - 2)) + PMember->getMod(xi::Mod::HPHEAL);
        ahead.mp += 12 + (n - 2) * (1 + PMember->getMod(xi::Mod::CLEAR_MIND)) + PMember->getMod(xi::Mod::MPHEAL);
    }
    ahead.hp = std::min(ahead.hp, PMember->GetMaxHP());
    ahead.mp = std::min(ahead.mp, PMember->GetMaxMP());
    return ahead;
}

// The cheapest Cure the healer has the MP for that heals `missing`, or with `biggest` the largest it can afford:
// amount and MP cost, { 0, 0 } for none. Amounts as heal() in bot_combat.cpp reckons them.
auto cureFor(CBattleEntity* PHealer, const int32 missing, const int32 mp, const bool biggest = false) -> std::pair<int32, int32>
{
    static constexpr std::array<std::pair<SpellID, int32>, 4> cures = { { { SpellID::Cure, 40 }, { SpellID::Cure_II, 100 }, { SpellID::Cure_III, 220 }, { SpellID::Cure_IV, 450 } } };

    std::pair<int32, int32> found{};
    for (const auto& [id, amount] : cures)
    {
        const auto* PSpell = spell::GetSpell(id);
        if (PSpell == nullptr || !spell::CanUseSpell(PHealer, id) || mp < PSpell->getMPCost())
        {
            continue;
        }
        found = { amount, PSpell->getMPCost() };
        if (!biggest && amount >= missing)
        {
            return found;
        }
    }
    return biggest ? found : std::pair<int32, int32>{};
}

// Seconds until the monster the party is on goes down, from how fast it has been dropping; nothing until that shows.
auto killSeconds(Pull& pull, const CMobEntity* PMob, const timer::time_point tick) -> std::optional<float>
{
    if (pull.fightMob != EntityId(PMob))
    {
        pull.fightMob     = EntityId(PMob);
        pull.fightSeenAt  = tick;
        pull.fightSeenHPP = PMob->GetHPP();
        return std::nullopt;
    }
    const float watched = std::chrono::duration<float>(tick - pull.fightSeenAt).count();
    const int   dropped = pull.fightSeenHPP - PMob->GetHPP();
    if (watched < kWatchFight || dropped < 5)
    {
        return std::nullopt;
    }
    return PMob->GetHPP() * watched / static_cast<float>(dropped);
}

auto percent(const int32 value, const int32 max) -> int32
{
    return max > 0 ? value * 100 / max : 100;
}

// What the puller waits on before going out: a key (each reason is said once) and the party line, or an empty key when
// it's ready. Judged the way a puller times it: on where the party will be when a monster `trip` seconds out gets to
// camp. The fight at camp will be over, resting and Refresh go on meanwhile, the healer tops up anyone a Cure short,
// and the main tank needs no more than one of its biggest. A fight with no end in sight has a key but no line.
auto waitingOn(CCharEntity* PChar, Pull& pull, const float trip, const timer::time_point tick) -> std::pair<std::string, std::string>
{
    const auto mobs = partyMobs(PChar);
    if (mobs.size() > 1)
    {
        return { "fight", "Waiting: the party has more than one monster on it." };
    }
    float fighting = 0.0f;
    if (mobs.size() == 1 && mobs.front()->GetHPP() > kLeaveAtHPP)
    {
        const auto left = killSeconds(pull, mobs.front(), tick);
        if (!left || *left + kFightSpare > trip)
        {
            return { "busy", {} };
        }
        fighting = *left;
    }
    auto* PTank = mainTank(PChar);
    if (!mobs.empty() && PTank != nullptr && PTank->GetHPP() < kTankPullHPP)
    {
        return { fmt::format("tank:{}", PTank->getName()), fmt::format("Waiting on {}'s HP ({}%) before pulling another.", PTank->getName(), PTank->GetHPP()) };
    }

    std::vector<std::pair<CBattleEntity*, Outlook>> party;
    for (auto* PMember : PChar->PParty->members)
    {
        const auto name = PMember->getName();
        if (PMember->isDead())
        {
            return { fmt::format("dead:{}", name), fmt::format("Waiting: {} is down.", name) };
        }
        if (PMember->getZone() != PChar->getZone())
        {
            return { fmt::format("away:{}", name), fmt::format("Waiting for {} to get here.", name) };
        }
        if (PMember->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Weakness))
        {
            return { fmt::format("weak:{}", name), fmt::format("Waiting for {}'s weakness to wear off.", name) };
        }
        party.emplace_back(PMember, outlook(PMember, PMember == PChar, fighting, trip, tick));
    }

    auto*      PHealer = partyHealer(PChar);
    const auto healer  = std::ranges::find(party, PHealer, &std::pair<CBattleEntity*, Outlook>::first);
    int32      spare   = healer != party.end() ? healer->second.mp : 0; // the healer's MP, less the cures it'll cast
    const auto later   = [&](const int32 now, const int32 then)
    { return trip > 0.0f && then != now ? fmt::format("{}% now, {}% when a pull gets here", now, then) : fmt::format("{}%", now); };

    for (const auto& [PMember, ahead] : party)
    {
        const auto name    = PMember->getName();
        const auto maxHP   = PMember->GetMaxHP();
        const auto missing = PMember == PTank || PHealer == nullptr ? maxHP - ahead.hp : maxHP * kRestHPP / 100 - ahead.hp;
        bool       fine    = PMember == PTank ? percent(ahead.hp, maxHP) >= kTankPullHPP : percent(ahead.hp, maxHP) >= kRestHPP;
        if (PHealer != nullptr && missing > 0)
        {
            // The tank needs to be within one big Cure of full; anyone else gets one up to the rest line.
            const auto [amount, cost] = PMember == PTank ? cureFor(PHealer, missing, spare, true) : cureFor(PHealer, missing, spare);
            fine = amount >= missing;
            if (fine && (PMember != PTank || percent(ahead.hp, maxHP) < kTankPullHPP))
            {
                spare -= cost;
            }
        }
        if (!fine)
        {
            return { fmt::format("HP:{}", name), fmt::format("Waiting on {}'s HP ({}) before pulling.", name, later(PMember->GetHPP(), percent(ahead.hp, maxHP))) };
        }
    }
    for (const auto& [PMember, ahead] : party)
    {
        const auto mp = PMember == PHealer ? spare : ahead.mp;
        if (PMember->GetMaxMP() > 0 && percent(mp, PMember->GetMaxMP()) < (PMember == PHealer ? kHealerPullMPP : kRestMPP))
        {
            return { fmt::format("MP:{}", PMember->getName()),
                     fmt::format("Waiting on {}'s MP ({}) before pulling.", PMember->getName(), later(PMember->GetMPP(), percent(mp, PMember->GetMaxMP()))) };
        }
    }
    return {};
}

// What this pull took against the estimate, folded into the party's pace.
void paced(Pull& pull, const timer::time_point tick)
{
    if (pull.expected <= 0.0f)
    {
        return;
    }
    const float took = std::chrono::duration<float>(tick - pull.leftAt).count();
    pull.pace        = std::clamp(0.7f * pull.pace + 0.3f * took / pull.expected, 0.6f, 2.5f);
    ShowInfoFmt("bots: the pull took {:.0f}s, {:.0f}s by distance; this party's pace is now {:.2f}", took, pull.expected, pull.pace);
    pull.expected = 0.0f;
}

// More than one monster on the party, or someone going down: come back and help.
auto danger(CCharEntity* PChar) -> bool
{
    return partyMobs(PChar).size() > 1 ||
           std::ranges::any_of(PChar->PParty->members, [](const CBattleEntity* PMember) { return !PMember->isDead() && PMember->GetHPP() < kTroubleHPP; });
}

auto choiceFor(const CCharEntity* PChar) -> const Choice*
{
    const auto* PLeader = leaderOfParty(PChar);
    const auto  it      = PLeader != nullptr ? choices.find(PLeader->id) : choices.end();
    return it != choices.end() && it->second.zone == PChar->getZone() ? &it->second : nullptr;
}

// Off the party's list, or on the list of ones to leave alone.
auto unwanted(const CCharEntity* PChar, const CMobEntity* PMob) -> bool
{
    const auto* choice = choiceFor(PChar);
    if (choice == nullptr)
    {
        return false;
    }
    const auto name = prettyName(PMob->getName());
    return (!choice->only.empty() && !std::ranges::contains(choice->only, name)) || std::ranges::contains(choice->avoid, name);
}

// Named by the party: pulled at any level.
auto asked(const CCharEntity* PChar, const CMobEntity* PMob) -> bool
{
    const auto* choice = choiceFor(PChar);
    return choice != nullptr && std::ranges::contains(choice->only, prettyName(PMob->getName()));
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
auto unpullable(const CCharEntity* PChar, CMobEntity* PMob, const position_t& camp, const int level, const int weakest, const int toughest) -> const char*
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
    if (unwanted(PChar, PMob))
    {
        return "not asked for";
    }
    if ((gap < weakest || gap > toughest) && !asked(PChar, PMob))
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
    return unpullable(PChar, PMob, camp, level, weakest, toughestGap(PChar)) == nullptr;
}

// The nearest suitable monster to camp: within 50 yalms if there is one, else within 180; near the party's level (weaker
// only when there's nothing in the usual range); one standing alone over one that might link. Not one it just gave up on.
auto pickTarget(CCharEntity* PChar, const Pull& pull, const timer::time_point tick) -> CMobEntity*
{
    const auto& camp  = pull.camp;
    const int   level = levelOf(PChar);
    for (const auto& [radius, weakest] : { std::pair{ kPullNear, kWeakestGap }, std::pair{ kPullNear, kFallbackGap }, std::pair{ kPullLimit, kWeakestGap },
                                           std::pair{ kPullLimit, kFallbackGap } })
    {
        CMobEntity* PBest       = nullptr;
        bool        bestCrowded = true;
        PChar->loc.zone->ForEachMob(
            [&](CMobEntity* PMob)
            {
                if (!pullable(PChar, PMob, camp, level, weakest) || distance(camp, PMob->loc.p) > radius ||
                    (tick < pull.gaveUpUntil && EntityId(PMob) == pull.gaveUpOn))
                {
                    return;
                }

                bool crowded = false;
                PChar->loc.zone->ForEachMob(
                    [&](CMobEntity* POther)
                    {
                        crowded = crowded || (POther != PMob && POther->isAlive() && POther->PAI->IsSpawned() && distance(POther->loc.p, PMob->loc.p) < kLinkRadius);
                    });

                if (PBest == nullptr || (bestCrowded && !crowded) || (crowded == bestCrowded && distance(camp, PMob->loc.p) < distance(camp, PBest->loc.p)))
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
    return nullptr;
}

auto loosePullable(const CCharEntity* PChar, CMobEntity* PMob) -> bool
{
    const auto* choice = choiceFor(PChar);
    return PMob->isAlive() && PMob->PAI->IsSpawned() && !PMob->PAI->IsEngaged() && !PMob->m_OwnerID.isSet() && PMob->m_Type == xi::MobType::Normal &&
           PMob->PMaster == nullptr && !unwanted(PChar, PMob) && (choice == nullptr || choice->only.empty() || asked(PChar, PMob));
}

auto nearestAny(CCharEntity* PChar, const Pull& pull) -> CMobEntity*
{
    CMobEntity* PBest = nullptr;
    PChar->loc.zone->ForEachMob(
        [&](CMobEntity* PMob)
        {
            if (loosePullable(PChar, PMob) && (PBest == nullptr || distance(pull.camp, PMob->loc.p) < distance(pull.camp, PBest->loc.p)))
            {
                PBest = PMob;
            }
        });
    return PBest;
}

auto runTime(const float yalms) -> std::chrono::milliseconds
{
    return std::chrono::milliseconds(static_cast<int64>(yalms / kRunSpeed * 1000.0f));
}

auto wordsOf(const std::string& text) -> std::vector<std::string>
{
    std::vector<std::string> words;
    std::string              word;
    for (const char c : text + ' ')
    {
        if (std::isalpha(static_cast<unsigned char>(c)))
        {
            word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        else if (!word.empty())
        {
            words.push_back(word);
            word.clear();
        }
    }
    return words;
}

auto singular(std::string word) -> std::string
{
    if (word.size() > 3 && word.ends_with('s') && !word.ends_with("ss"))
    {
        word.pop_back();
    }
    return word;
}

// Edits apart, a swapped pair counting as one: "seige" is one from "siege".
auto typos(const std::string& a, const std::string& b) -> std::size_t
{
    std::vector<std::vector<std::size_t>> d(a.size() + 1, std::vector<std::size_t>(b.size() + 1));
    for (std::size_t i = 0; i <= a.size(); ++i)
    {
        d[i][0] = i;
    }
    for (std::size_t j = 0; j <= b.size(); ++j)
    {
        d[0][j] = j;
    }
    for (std::size_t i = 1; i <= a.size(); ++i)
    {
        for (std::size_t j = 1; j <= b.size(); ++j)
        {
            d[i][j] = std::min({ d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1) });
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
            {
                d[i][j] = std::min(d[i][j], d[i - 2][j - 2] + 1);
            }
        }
    }
    return d[a.size()][b.size()];
}

// A word of the line against a word of a monster's name: plurals and a typo in a longer word allowed.
auto sameWord(const std::string& said, const std::string& name) -> bool
{
    const auto a = singular(said);
    const auto b = singular(name);
    return a == b || (std::min(a.size(), b.size()) >= 4 && typos(a, b) <= 1);
}

// The kinds of monster a line names: "Siege Bats" names Siege Bat alone, a bare "bats" every kind of bat.
auto namedKinds(const std::string& text, const std::map<std::string, std::string>& kinds) -> std::vector<std::string>
{
    // The instruction's own words never name a monster ("only" is one typo from "orly").
    static const std::set<std::string> kInstruction = { "pull", "pulls", "pulling", "only", "just", "dont", "stop", "more", "the", "and", "please", "plz", "pls" };
    std::vector<std::string> said;
    std::ranges::copy_if(wordsOf(text), std::back_inserter(said), [](const std::string& word) { return !kInstruction.contains(word); });
    const auto saidWord = [&](const std::string& word) { return std::ranges::any_of(said, [&](const std::string& s) { return sameWord(s, word); }); };

    std::vector<std::string> named;
    std::set<std::string>    covered; // last words of kinds named in full
    for (const auto& kind : kinds | std::views::keys)
    {
        const auto parts = wordsOf(kind);
        if (parts.size() > 1 && std::ranges::all_of(parts, saidWord))
        {
            named.push_back(kind);
            covered.insert(singular(parts.back()));
        }
    }
    for (const auto& kind : kinds | std::views::keys)
    {
        const auto parts = wordsOf(kind);
        if (!parts.empty() && !std::ranges::contains(named, kind) && !covered.contains(singular(parts.back())) && saidWord(parts.back()))
        {
            named.push_back(kind);
        }
    }
    return named;
}

// "A", "A and B", "A, B and C".
auto listed(const std::vector<std::string>& names) -> std::string
{
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        out += (i == 0 ? "" : i + 1 == names.size() ? " and " : ", ") + names[i];
    }
    return out;
}

void finish(Pull& pull, const timer::time_point tick, const char* why)
{
    const auto* PPuller = zoneutils::GetChar(pull.pullerId);
    ShowInfoFmt("bots: pull by {} ended: {}", PPuller != nullptr ? PPuller->getName() : std::to_string(pull.pullerId), why);
    pull.announced = false;
    pull.force     = false;
    pull.stage     = Stage::Waiting;
    pull.mob       = {};
    pull.stageAt   = tick;
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

    static constexpr std::array<const char*, 3> stages = { "waiting at camp", "heading out", "bringing it back" };
    std::string puller = "nobody pulling";
    if (pull->pullerId != 0)
    {
        const auto* PPuller = zoneutils::GetChar(pull->pullerId);
        const auto* PMob    = pull->mob.resolve<CMobEntity>();
        puller = fmt::format("puller {} {}{}", PPuller ? PPuller->getName() : std::string("?"), stages[static_cast<std::size_t>(pull->stage)],
                             PMob ? fmt::format(" ({} lv{})", prettyName(PMob->getName()), PMob->GetMLevel()) : std::string());
    }
    const bool here   = campSpot(PChar).has_value();
    const auto choice = pullChoiceText(PChar);
    return fmt::format("Camp: {:.0f},{:.0f}, leader {:.0f}y from it{}; {}{}", pull->camp.x, pull->camp.z, distance(PLeader->loc.p, pull->camp),
                       here ? "" : " (paused)", puller, choice.empty() ? "" : "; " + choice);
}

// Naming a puller keeps the camp (only "camp here" moves it), and naming the one already pulling changes nothing.
void setPuller(CBattleEntity* PLeader, const uint32 pullerId)
{
    const auto it = pulls.find(PLeader->id);
    if (it != pulls.end() && it->second.zone == PLeader->getZone())
    {
        if (it->second.pullerId == pullerId)
        {
            return;
        }
        auto& pull     = it->second;
        pull.pullerId  = pullerId;
        pull.stage     = Stage::Waiting;
        pull.mob       = {};
        pull.announced = false;
        pull.waitSaid.clear();
    }
    else
    {
        pulls[PLeader->id] = Pull{ .pullerId = pullerId, .zone = PLeader->getZone(), .camp = PLeader->loc.p };
    }
    const auto* PPuller = zoneutils::GetChar(pullerId);
    ShowInfoFmt("bots: {} is pulling for {}'s party", PPuller != nullptr ? PPuller->getName() : std::to_string(pullerId), PLeader->getName());
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
    choices.erase(PLeader->id);
}

auto pullerOf(const CCharEntity* PChar) -> uint32
{
    const auto* pull = pullFor(PChar);
    return pull != nullptr ? pull->pullerId : 0;
}

auto pullChoiceText(const CCharEntity* PChar) -> std::string
{
    const auto* choice = choiceFor(PChar);
    if (choice == nullptr)
    {
        return {};
    }
    std::string out;
    if (!choice->only.empty())
    {
        out += fmt::format("Pulling only {}.", listed(choice->only));
    }
    if (!choice->avoid.empty())
    {
        out += fmt::format("{}Not pulling {}.", out.empty() ? "" : " ", listed(choice->avoid));
    }
    return out;
}

// "pull bats only", "Siege Bats and Borer Beetles ONLY", "don't pull soldiers", "pull anything": what the party's puller goes
// for, read from the line here because the model took "bats only" for "stop pulling". The note for the bot that answers, or
// nothing when the line isn't about what to pull.
auto pullChoice(const std::string& text, CCharEntity* PSpeaker) -> std::optional<std::string>
{
    auto* PLeader = PSpeaker != nullptr ? leaderOfParty(PSpeaker) : nullptr;
    if (PLeader == nullptr || PSpeaker->loc.zone == nullptr)
    {
        return std::nullopt;
    }

    const auto said = lowerCase(text);
    if (saysPhrase(said, { "pull anything", "pull everything", "pull whatever", "anything is fine", "anything goes", "any monster", "any mob" }))
    {
        if (choices.erase(PLeader->id) == 0)
        {
            return std::nullopt;
        }
        return "You were just told to pull anything again, and you will.";
    }

    std::map<std::string, std::string> kinds; // each kind of monster in the zone, lower case to as shown
    PSpeaker->loc.zone->ForEachMob(
        [&](CMobEntity* PMob)
        {
            if (PMob->m_Type == xi::MobType::Normal && PMob->PMaster == nullptr)
            {
                kinds.emplace(lowerCase(prettyName(PMob->getName())), prettyName(PMob->getName()));
            }
        });
    std::vector<std::string> named;
    for (const auto& kind : namedKinds(said, kinds))
    {
        named.push_back(kinds.at(kind));
    }
    if (named.empty())
    {
        return std::nullopt;
    }

    const bool avoid = saysPhrase(said, { "don't pull", "dont pull", "do not pull", "stop pulling", "no more", "avoid", "skip", "leave the", "not the", "except", "but not" });
    if (!avoid && !saysPhrase(said, { "only", "just", "pull", "pulls", "pulling", "stick to", "go for" }))
    {
        return std::nullopt;
    }

    auto& choice = choices[PLeader->id];
    if (choice.zone != PSpeaker->getZone())
    {
        choice = Choice{ .zone = PSpeaker->getZone() };
    }
    ShowInfoFmt("bots: {}'s party {} {}", PLeader->getName(), avoid ? "leaves alone" : "pulls only", listed(named));

    if (avoid)
    {
        for (const auto& kind : named)
        {
            if (!std::ranges::contains(choice.avoid, kind))
            {
                choice.avoid.push_back(kind);
            }
            std::erase(choice.only, kind);
        }
        return fmt::format("You were just told not to pull {} from now on, and you won't.", listed(named));
    }
    choice.only = named;
    std::erase_if(choice.avoid, [&](const std::string& kind) { return std::ranges::contains(named, kind); });

    // Pulled at any level, since they were asked for; say so if they're well above the party.
    int lowest = 255;
    int highest = 0;
    PSpeaker->loc.zone->ForEachMob(
        [&](CMobEntity* PMob)
        {
            if (std::ranges::contains(named, prettyName(PMob->getName())))
            {
                lowest  = std::min<int>(lowest, PMob->GetMLevel());
                highest = std::max<int>(highest, PMob->GetMLevel());
            }
        });
    const int party = levelOf(PSpeaker);
    const auto tough = highest > party + toughestGap(PSpeaker)
                           ? fmt::format(" They're level {} to {} and the party's highest is {}, so they'll be tough.", lowest, highest, party)
                           : std::string();
    return fmt::format("You were just told to pull only {} from now on, and you will.{}", listed(named), tough);
}

// "come back", "wrong way": the puller drops the monster it's heading for and comes back to camp. Its note, or nothing.
auto recallPuller(const std::string& text, CCharEntity* PSpeaker) -> std::optional<std::string>
{
    auto* pull = PSpeaker != nullptr ? pullFor(PSpeaker) : nullptr;
    if (pull == nullptr || pull->pullerId == 0 || pull->stage == Stage::Waiting)
    {
        return std::nullopt;
    }

    const auto said = lowerCase(text);
    const bool back = saysPhrase(said, { "come back", "get back", "back to camp", "return to camp", "go back", "wrong way", "wrong direction", "where are you going",
                                         "where you going", "stop running" }) ||
                      (said.find("wrong") != std::string::npos && said.find("way") != std::string::npos);
    if (!back)
    {
        return std::nullopt;
    }

    if (pull->stage == Stage::Returning)
    {
        return "You were just called back; you're already bringing the monster to camp.";
    }

    const auto tick = timer::now();
    if (auto* PMob = pull->mob.resolve<CMobEntity>())
    {
        pull->gaveUpOn    = EntityId(PMob);
        pull->gaveUpUntil = tick + kGiveUpFor;
    }
    pull->heldUntil = tick + kRecallHold;
    finish(*pull, tick, "called back");
    return "You were just called back, so you dropped that pull and are coming back to camp.";
}

// Why the puller has nothing to go for right now, for its answer; empty when there is something.
// "pull anyway", "pull now", "don't wait" from the party leader: the puller goes now, ready or not, and takes anything the party
// hasn't ruled out if nothing passes the usual picks. Its note, or nothing.
auto pullAnyway(const std::string& text, CCharEntity* PSpeaker) -> std::optional<std::string>
{
    auto* pull = PSpeaker != nullptr && PSpeaker->PParty != nullptr && PSpeaker->PParty->GetLeader() == PSpeaker ? pullFor(PSpeaker) : nullptr;
    if (pull == nullptr || pull->pullerId == 0)
    {
        return std::nullopt;
    }

    const auto said = lowerCase(text);
    const bool now  = (saysPhrase(said, { "pull", "pulling", "go" }) && saysPhrase(said, { "anyway", "anyways", "regardless", "already", "now" })) ||
                     saysPhrase(said, { "just pull", "don't wait", "dont wait", "do not wait", "stop waiting" });
    if (!now)
    {
        return std::nullopt;
    }

    pull->force     = true;
    pull->heldUntil = {};
    if (pull->stage != Stage::Waiting)
    {
        return "The leader just told you to pull anyway; you're already on it.";
    }
    return "The leader just told you to pull anyway, so you're going now, ready or not.";
}

// Why nothing can be pulled, as a clause a player can read ("of the 37 Eschan Wasp here, 30 more than 180 yalms from camp"); empty
// when something can.
auto pullBlocker(CCharEntity* PPuller) -> std::string
{
    auto* PLeader = PPuller != nullptr && PPuller->PParty != nullptr ? leaderOfParty(PPuller) : nullptr;
    if (PLeader == nullptr || PPuller->loc.zone == nullptr)
    {
        return {};
    }

    const auto* pull     = pullFor(PPuller);
    const auto  camp     = pull != nullptr ? pull->camp : PLeader->loc.p;
    const int   level    = levelOf(PPuller);
    const int   toughest = toughestGap(PPuller);
    const auto* choice   = choiceFor(PPuller);
    const bool  named    = choice != nullptr && !choice->only.empty();

    int                        total = 0;
    bool                       any   = false;
    std::map<std::string, int> reasons;
    PPuller->loc.zone->ForEachMob(
        [&](CMobEntity* PMob)
        {
            const int gap = PMob->GetMLevel() - level;
            if (PMob->m_Type != xi::MobType::Normal || PMob->PMaster != nullptr ||
                (named ? !asked(PPuller, PMob) : unwanted(PPuller, PMob) || gap < kFallbackGap || gap > toughest))
            {
                return;
            }
            ++total;
            const auto* why = unpullable(PPuller, PMob, camp, level, kFallbackGap, toughest);
            any             = any || why == nullptr;
            ++reasons[why != nullptr ? why : "ok"];
        });
    if (any)
    {
        return {};
    }

    const auto what = named ? listed(choice->only) : fmt::format("monsters level {} to {}", std::max(1, level + kFallbackGap), level + toughest);
    if (total == 0)
    {
        return fmt::format("there are no {} in this zone", what);
    }
    std::vector<std::string> parts;
    for (const auto& [why, n] : reasons)
    {
        parts.push_back(fmt::format("{} {}", n, why == "busy or claimed" ? "claimed or already fighting" : why == "too far" ? fmt::format("more than {:.0f} yalms from camp", kPullLimit) : why));
    }
    return fmt::format("of the {} {} here, {}", total, what, fmt::join(parts, ", "));
}

auto pullOutlook(CCharEntity* PPuller) -> std::string
{
    const auto blocker = pullBlocker(PPuller);
    return blocker.empty() ? std::string() : fmt::format("But there's nothing for you to pull right now: {}. Say so plainly; don't say you're pulling.", blocker);
}

// What the party's puller is up to, for its answer to a line about pulling.
auto pullReport(CCharEntity* PPuller) -> std::string
{
    const auto* pull = PPuller != nullptr ? pullFor(PPuller) : nullptr;
    if (pull == nullptr || pull->pullerId != PPuller->id)
    {
        return {};
    }
    const auto* PMob = pull->mob.resolve<CMobEntity>();
    switch (pull->stage)
    {
        case Stage::Outbound:
            return PMob ? fmt::format("You're the party's puller, on your way to a {} (level {}), {:.0f} yalms from camp.", prettyName(PMob->getName()),
                                      PMob->GetMLevel(), distance(PMob->loc.p, pull->camp))
                        : "You're the party's puller, out looking for a monster.";
        case Stage::Returning:
            return PMob ? fmt::format("You're the party's puller, bringing a {} back to camp.", prettyName(PMob->getName()))
                        : "You're the party's puller, on your way back to camp.";
        case Stage::Waiting:
            break;
    }
    const auto outlook = pullOutlook(PPuller);
    return outlook.empty() ? "You're the party's puller, at camp; you go for the next one once the party has its HP and MP back."
                           : "You're the party's puller, at camp. " + outlook;
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

    // Why it's holding off, said once each until the next pull.
    const auto waitSay = [&](const std::string& key, const std::string& line)
    {
        if (!line.empty() && pull->waitSaid.insert(key).second)
        {
            ShowInfoFmt("bots: {} waits: {}", PChar->getName(), line);
            callout(bot, line, false);
        }
    };

    // The leader away from camp (or a town): no new pulls, and one on its way out is dropped. One already coming back still comes.
    const auto camp = pull->camp;
    const bool town = (static_cast<uint16>(PChar->loc.zone->GetTypeMask()) & static_cast<uint16>(xi::ZoneType::City)) != 0;
    if (town || pull->zone != PChar->getZone() || distance(PLeader->loc.p, camp) > kCampDrift)
    {
        if (pull->stage == Stage::Outbound)
        {
            finish(*pull, tick, "leader left camp");
        }
        if (pull->stage == Stage::Waiting)
        {
            if (!town && pull->zone == PChar->getZone())
            {
                waitSay("away", fmt::format("Holding pulls while {} is away from camp.", PLeader->getName()));
            }
            return false;
        }
    }

    auto* PMob = pull->mob.resolve<CMobEntity>();

    switch (pull->stage)
    {
        case Stage::Waiting:
        {
            if (!pull->force && tick < pull->heldUntil)
            {
                return false;
            }

            // Ready is judged for when this one would get to camp; with nothing to pull, on the party as it is.
            PMob = pickTarget(PChar, *pull, tick);
            if (PMob == nullptr && pull->force)
            {
                PMob = nearestAny(PChar, *pull);
            }
            if (!pull->force)
            {
                const float trip = PMob != nullptr ? tripSeconds(PChar, *pull, PMob) * pull->pace : 0.0f;
                if (const auto [key, line] = waitingOn(PChar, *pull, trip, tick); !key.empty())
                {
                    waitSay(key, line);
                    return false;
                }
            }
            if (PMob == nullptr)
            {
                if (pull->force)
                {
                    pull->force = false;
                    waitSay("none at all", "There's nothing up to pull at all.");
                }
                else if (const auto blocker = pullBlocker(PChar); !blocker.empty())
                {
                    waitSay("nothing", fmt::format("Not pulling: {}.", blocker));
                }
                return false;
            }

            pull->mob      = EntityId(PMob);
            pull->stage    = Stage::Outbound;
            pull->stageAt  = tick;
            pull->deadline = tick + runTime(distance(PChar->loc.p, PMob->loc.p)) * 2 + kSpareTime;
            pull->leftAt   = tick;
            pull->expected = tripSeconds(PChar, *pull, PMob);
            ShowInfoFmt("bots: {} is going for {} (level {}, {:.0f} yalms from camp, back in ~{:.0f}s{})", PChar->getName(), PMob->getName(), PMob->GetMLevel(),
                        distance(camp, PMob->loc.p), pull->expected * pull->pace, pull->force ? ", told to pull anyway" : "");
            [[fallthrough]];
        }
        case Stage::Outbound:
        {
            // Ours once it's coming for the puller or fighting anyone in the party.
            const bool partyOwns = PMob != nullptr && PMob->m_OwnerID.isSet() &&
                                   std::ranges::any_of(PChar->PParty->members, [&](const CBattleEntity* PMember) { return PMember->id == PMob->m_OwnerID.UniqueNo; });
            const bool ours = PMob != nullptr && (PMob->GetBattleTarget() == PChar || (PMob->PAI->IsEngaged() && inPartyOf(PChar, PMob->GetBattleTarget())));
            if (ours)
            {
                pull->force = false;
                pull->waitSaid.clear();
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

            // Gone, taken by someone else, wandered out of reach, too long getting there, or trouble back at camp: back to waiting.
            const bool trouble = !pull->force && danger(PChar);
            const char* why    = PMob == nullptr || !PMob->isAlive() || !PMob->PAI->IsSpawned()        ? "target gone"
                                 : (PMob->m_OwnerID.isSet() && !partyOwns) || PMob->PAI->IsEngaged() ? "target taken"
                                 : !pull->force && distance(camp, PMob->loc.p) > kPullLimit + 10.0f   ? "target wandered off"
                                 : tick > pull->deadline                                              ? "couldn't get to it in time"
                                 : trouble                                                            ? "trouble at camp"
                                                                                                      : nullptr;
            if (why != nullptr)
            {
                if (PMob != nullptr && tick > pull->deadline)
                {
                    pull->gaveUpOn    = EntityId(PMob);
                    pull->gaveUpUntil = tick + kGiveUpFor;
                }
                if (trouble)
                {
                    waitSay("trouble", "Coming back - trouble at camp.");
                }
                finish(*pull, tick, why);
                return false;
            }

            // Leaving the last one at camp to the others.
            if (PChar->PAI->IsEngaged() && PChar->GetBattleTarget() != PMob)
            {
                disengage(bot, packets);
            }

            // Out to opening range, then open with whatever reaches; a melee-only puller walks up and swings.
            const float range = pullRange(PChar);
            const float reach = range > 0.0f ? range : PChar->GetMeleeRange(PMob) * 0.8f;
            if (distance(PChar->loc.p, PMob->loc.p) > reach)
            {
                if (!PChar->PAI->IsEngaged() && canAct(PChar) && distance(PChar->loc.p, PMob->loc.p) > 30.0f && speedUp(bot, packets, tick))
                {
                    return true;
                }
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
                paced(*pull, tick);
                finish(*pull, tick, "the tank has it");
                return false;
            }
            // It lost interest on the way: go back and open on it again.
            if (PMob->GetBattleTarget() == nullptr && !PMob->PAI->IsEngaged() && tick <= pull->deadline)
            {
                pull->stage = Stage::Outbound;
                ShowInfoFmt("bots: {} lost {} on the way back; pulling it again", PChar->getName(), PMob->getName());
                return true;
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
            paced(*pull, tick);
            finish(*pull, tick, "back at camp");
            return false;
        }
    }
    return false;
}

} // namespace bots
