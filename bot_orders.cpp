//
// Party requests in chat ("provoke that", "protect please", "cure Ysolde", "stop", "stay here").
//
//   Plain word matching, not the model: the model would agree to anything. The bot
//   that takes the request is told in its chat prompt, so its reply matches what it does.
//

#include "bots.h"

#include "map/ability.h"
#include "map/entities/char_entity.h"
#include "map/party.h"
#include "map/spell.h"
#include "map/utils/charutils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

namespace bots
{

namespace
{

using xi::Job;

auto lowered(std::string text) -> std::string
{
    std::ranges::transform(text, text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Whole-word (or whole-phrase) match.
auto says(const std::string& text, const std::string_view phrase) -> bool
{
    for (auto at = text.find(phrase); at != std::string::npos; at = text.find(phrase, at + 1))
    {
        const bool startOk = at == 0 || !std::isalpha(static_cast<unsigned char>(text[at - 1]));
        const auto end     = at + phrase.size();
        const bool endOk   = end >= text.size() || !std::isalpha(static_cast<unsigned char>(text[end]));
        if (startOk && endOk)
        {
            return true;
        }
    }
    return false;
}

auto saysAny(const std::string& text, const std::initializer_list<std::string_view> phrases) -> bool
{
    return std::ranges::any_of(phrases, [&](const std::string_view phrase) { return says(text, phrase); });
}

struct Request
{
    OrderKind            kind{};
    std::vector<SpellID> spells;
    bool                 onMonster{};
    const char*          what{};
    bool                 needsName{}; // only when a bot is named in the line
};

auto parse(const std::string& text) -> std::optional<Request>
{
    const auto t = lowered(text);

    // Checked in order: "heal me" is a cure, "heal up" is a rest.
    if (saysAny(t, { "provoke", "voke", "grab hate", "get hate", "take hate", "pull hate" }))
    {
        return Request{ OrderKind::Provoke, {}, true, "provoke the monster" };
    }
    if (saysAny(t, { "stop pulling", "no more pulls", "dont pull", "don't pull", "i'll pull", "ill pull", "i will pull", "i pull", "let me pull" }))
    {
        return Request{ OrderKind::StopPull, {}, false, "stop pulling" };
    }
    if (saysAny(t, { "start pulling", "pull for us", "you pull", "somebody pull", "someone pull", "anybody pull", "anyone pull", "need a puller",
                     "who's pulling", "whos pulling", "who can pull", "can you pull", "could you pull", "will you pull", "please pull", "pull please",
                     "go pull", "be the puller", "you're the puller", "youre the puller", "be puller", "pull plz", "pull pls", "you're pulling", "youre pulling" }))
    {
        return Request{ OrderKind::Pull, {}, false, "pull monsters back to camp for the party" };
    }
    // A bare "pull" only counts with a bot's name in the line ("Kakaru pull"), or "nice pull" would hand out the job.
    if (says(t, "pull"))
    {
        return Request{ OrderKind::Pull, {}, false, "pull monsters back to camp for the party", true };
    }
    if (saysAny(t, { "camp here", "set camp", "new camp", "camp is here", "camping here", "this is camp", "make camp", "here's camp", "heres camp",
                     "we camp here", "lets camp", "let's camp" }))
    {
        return Request{ OrderKind::Camp, {}, false, "treat this spot as camp" };
    }
    if (saysAny(t, { "raise", "raise me", "rr" }))
    {
        return Request{ OrderKind::Cast, { SpellID::Raise_II, SpellID::Raise }, false, "cast Raise" };
    }
    if (saysAny(t, { "cure", "cures", "heal me", "heals", "need heals", "curing" }))
    {
        return Request{ OrderKind::Cast, { SpellID::Cure_IV, SpellID::Cure_III, SpellID::Cure_II, SpellID::Cure }, false, "cast Cure" };
    }
    if (saysAny(t, { "protect", "pro", "protectra" }))
    {
        return Request{ OrderKind::Cast, { SpellID::Protect_III, SpellID::Protect_II, SpellID::Protect }, false, "cast Protect" };
    }
    if (saysAny(t, { "shell", "shellra" }))
    {
        return Request{ OrderKind::Cast, { SpellID::Shell_III, SpellID::Shell_II, SpellID::Shell }, false, "cast Shell" };
    }
    if (says(t, "haste"))
    {
        return Request{ OrderKind::Cast, { SpellID::Haste }, false, "cast Haste" };
    }
    if (says(t, "refresh"))
    {
        return Request{ OrderKind::Cast, { SpellID::Refresh }, false, "cast Refresh" };
    }
    if (says(t, "regen"))
    {
        return Request{ OrderKind::Cast, { SpellID::Regen }, false, "cast Regen" };
    }
    if (says(t, "dia"))
    {
        return Request{ OrderKind::Cast, { SpellID::Dia_II, SpellID::Dia }, true, "cast Dia on the monster" };
    }
    if (says(t, "slow"))
    {
        return Request{ OrderKind::Cast, { SpellID::Slow }, true, "cast Slow on the monster" };
    }
    if (saysAny(t, { "paralyze", "para" }))
    {
        return Request{ OrderKind::Cast, { SpellID::Paralyze }, true, "cast Paralyze on the monster" };
    }
    if (says(t, "silence"))
    {
        return Request{ OrderKind::Cast, { SpellID::Silence }, true, "cast Silence on the monster" };
    }
    if (says(t, "bind"))
    {
        return Request{ OrderKind::Cast, { SpellID::Bind }, true, "cast Bind on the monster" };
    }
    if (says(t, "sleep"))
    {
        return Request{ OrderKind::Cast, { SpellID::Sleep }, true, "cast Sleep on the monster" };
    }
    if (saysAny(t, { "blind", "blindness" }))
    {
        return Request{ OrderKind::Cast, { SpellID::Blind }, true, "cast Blind on the monster" };
    }
    if (saysAny(t, { "nuke", "nuke it", "burn it" }))
    {
        return Request{ OrderKind::Cast,
                        { SpellID::Thunder_II, SpellID::Blizzard_II, SpellID::Fire_II, SpellID::Aero_II, SpellID::Stone_II, SpellID::Water_II,
                          SpellID::Thunder, SpellID::Blizzard, SpellID::Fire, SpellID::Aero, SpellID::Stone, SpellID::Water },
                        true, "nuke the monster" };
    }
    if (says(t, "assist"))
    {
        return Request{ OrderKind::Assist, {}, false, "assist" };
    }
    if (saysAny(t, { "attack", "engage", "kill it", "get it", "help me" }))
    {
        return Request{ OrderKind::Attack, {}, true, "attack the monster" };
    }
    if (saysAny(t, { "stop", "back off", "disengage", "hold off", "dont attack", "don't attack" }))
    {
        return Request{ OrderKind::Stop, {}, false, "stop attacking" };
    }
    if (saysAny(t, { "rest", "heal up", "/heal", "take a break" }))
    {
        return Request{ OrderKind::Rest, {}, false, "rest" };
    }
    if (saysAny(t, { "stay", "wait here", "hold position", "stay here" }))
    {
        return Request{ OrderKind::Stay, {}, false, "stay where you are" };
    }
    if (saysAny(t, { "follow", "follow me", "come here", "lets go", "let's go", "come on" }))
    {
        return Request{ OrderKind::Follow, {}, false, "follow" };
    }
    return std::nullopt;
}

auto canCastAny(CCharEntity* PChar, const std::vector<SpellID>& spells) -> bool
{
    return std::ranges::any_of(spells,
                               [&](const SpellID id)
                               {
                                   return spell::CanUseSpell(PChar, id);
                               });
}

auto canProvoke(CCharEntity* PChar) -> bool
{
    return charutils::hasAbility(PChar, ABILITY_PROVOKE) || spell::CanUseSpell(PChar, SpellID::Flash);
}

constexpr uint8 kNever      = 255;
constexpr uint8 kElegyLevel = 39; // Battlefield Elegy

constexpr std::array<const char*, 23> jobNames = {
    "",         "Warrior", "Monk",   "White Mage", "Black Mage", "Red Mage", "Thief",        "Paladin", "Dark Knight", "Beastmaster", "Bard",       "Ranger",
    "Samurai",  "Ninja",   "Dragoon", "Summoner",  "Blue Mage",  "Corsair",  "Puppetmaster", "Dancer",  "Scholar",     "Geomancer",   "Rune Fencer",
};

// The level this job first gets what was asked for, or kNever.
auto learnLevel(const Job job, const Request& request) -> uint8
{
    if (request.kind == OrderKind::Provoke)
    {
        auto* PProvoke = ability::GetAbility(ABILITY_PROVOKE);
        uint8 level    = PProvoke && PProvoke->getJob() == job ? PProvoke->getLevel() : kNever;
        if (auto* PFlash = spell::GetSpell(SpellID::Flash); PFlash && PFlash->getJob(job) != 255)
        {
            level = std::min(level, PFlash->getJob(job));
        }
        return level;
    }

    uint8 level = kNever;
    for (const auto id : request.spells)
    {
        if (auto* PSpell = spell::GetSpell(id); PSpell && PSpell->getJob(job) != 255 && PSpell->getJob(job) > 0)
        {
            level = std::min(level, PSpell->getJob(job));
        }
    }
    return level;
}

// Why the bot can't, in plain facts it can repeat.
auto cantNote(CCharEntity* PChar, const Request& request) -> std::string
{
    const auto job     = PChar->GetMJob();
    const auto name    = static_cast<std::size_t>(job) < jobNames.size() ? jobNames[static_cast<std::size_t>(job)] : "adventurer";
    const auto learned = learnLevel(job, request);

    if (learned == kNever)
    {
        return fmt::format("You were just asked to {}, but you can't: a {} never gets that. Don't claim anything about what other jobs can do.", request.what, name);
    }
    if (learned > PChar->GetMLevel())
    {
        return fmt::format("You were just asked to {}, but you can't yet: {}s get it at level {} and you're level {}.", request.what, name, learned,
                           PChar->GetMLevel());
    }
    return fmt::format("You were just asked to {}, but you can't right now (not enough MP, or it's still recharging).", request.what);
}

// Healers first for cures and buffs, tanks first for hate.
// Pullers: a bard with Elegy (it pulls from range, slows the monster and sleeps links), then NIN, THF, RNG or COR, tanks.
auto preference(const Job job, const uint8 level, const OrderKind kind) -> int
{
    if (kind == OrderKind::Pull)
    {
        switch (job)
        {
            case Job::BRD: return level >= kElegyLevel ? 0 : 5;
            case Job::NIN: return 1;
            case Job::THF: return 2;
            case Job::RNG:
            case Job::COR: return 3;
            case Job::PLD:
            case Job::WAR: return 4;
            default: return 5;
        }
    }
    if (kind == OrderKind::Provoke)
    {
        return job == Job::PLD ? 0 : job == Job::WAR ? 1 : job == Job::NIN || job == Job::RUN ? 2 : 3;
    }
    return job == Job::WHM ? 0 : job == Job::RDM ? 1 : job == Job::SCH ? 2 : 3;
}

} // namespace

// "follow Ysolde", "assist Kakaru": the party member named after the keyword, else the speaker.
auto orderSubject(const std::string& text, const OrderKind kind, CCharEntity* PSpeaker) -> CBattleEntity*
{
    const auto        lower   = lowered(text);
    const std::string keyword = kind == OrderKind::Assist ? "assist" : "follow";
    const auto        at      = PSpeaker->PParty != nullptr ? lower.find(keyword) : std::string::npos;
    if (at == std::string::npos)
    {
        return PSpeaker;
    }

    CBattleEntity* PSubject = PSpeaker;
    std::size_t    nearest  = std::string::npos;
    for (auto* PMember : PSpeaker->PParty->members)
    {
        const auto found = lower.find(lowered(PMember->getName()), at + keyword.size());
        if (found != std::string::npos && found < nearest)
        {
            PSubject = PMember;
            nearest  = found;
        }
    }
    return PSubject;
}

// Lower is a better puller.
auto pullerRank(const Job job, const uint8 level) -> int
{
    return preference(job, level, OrderKind::Pull);
}

auto planOrder(const std::string& text, CCharEntity* PSpeaker, const std::vector<Bot*>& bots) -> std::vector<OrderPlan>
{
    const auto request = parse(text);
    if (!request || PSpeaker == nullptr || PSpeaker->PParty == nullptr)
    {
        return {};
    }

    const auto lower = lowered(text);

    // Bots in the speaker's party; the ones named in the line, if any.
    std::vector<Bot*> party;
    std::vector<Bot*> named;
    for (auto* bot : bots)
    {
        auto* PChar = bot->entity();
        if (PChar == nullptr || PChar->PParty != PSpeaker->PParty)
        {
            continue;
        }
        party.push_back(bot);
        if (says(lower, lowered(bot->name)))
        {
            named.push_back(bot);
        }
    }
    if (party.empty() || (request->needsName && named.empty()))
    {
        return {};
    }

    // Whom to follow or assist isn't one of the bots being told to.
    CBattleEntity* PSubject = nullptr;
    std::string    what     = request->what;
    if (request->kind == OrderKind::Follow || request->kind == OrderKind::Assist)
    {
        PSubject = orderSubject(text, request->kind, PSpeaker);
        std::erase_if(named, [&](const Bot* bot) { return bot->charId == PSubject->id; });
        what = fmt::format("{} {}", request->what, PSubject == PSpeaker ? "them" : PSubject->getName());
    }

    // Who the spell is for: a party member named in the line, everyone, or the speaker.
    std::vector<uint32> targets;
    if (!request->onMonster)
    {
        const bool everyone = saysAny(lower, { "everyone", "all", "party", "us", "protectra", "shellra" });
        for (const auto* PMember : PSpeaker->PParty->members)
        {
            const bool isBotNamedAsCaster = std::ranges::any_of(named, [&](const Bot* bot) { return bot->charId == PMember->id; });
            if (everyone || (says(lower, lowered(PMember->getName())) && !isBotNamedAsCaster))
            {
                targets.push_back(PMember->id);
            }
        }
        if (targets.empty())
        {
            targets.push_back(PSpeaker->id);
        }
    }

    const auto order = [&](const Bot* bot)
    {
        return Order{
            .kind      = request->kind,
            .spells    = request->spells,
            .targets   = targets,
            .speakerId = PSpeaker->id,
            .subject   = PSubject != nullptr ? PSubject->id : 0,
            .expires   = timer::now() + std::chrono::seconds(20),
        };
    };

    std::vector<OrderPlan> plans;

    // Party-wide requests go to everyone asked.
    if (request->kind == OrderKind::Attack || request->kind == OrderKind::Stop || request->kind == OrderKind::Rest ||
        request->kind == OrderKind::Stay || request->kind == OrderKind::Follow || request->kind == OrderKind::StopPull || request->kind == OrderKind::Camp ||
        request->kind == OrderKind::Assist)
    {
        for (auto* bot : named.empty() ? party : named)
        {
            if (PSubject == nullptr || bot->charId != PSubject->id)
            {
                plans.push_back(OrderPlan{ bot, order(bot), fmt::format("You were just asked to {} and you're doing it.", what) });
            }
        }
        return plans;
    }

    // One bot that can do it: a named one if it can, else the best suited.
    auto& pool    = named.empty() ? party : named;
    auto  capable = pool;
    std::erase_if(capable,
                  [&](Bot* bot)
                  {
                      auto* PChar = bot->entity();
                      if (request->kind == OrderKind::Pull)
                      {
                          return false;
                      }
                      return request->kind == OrderKind::Provoke ? !canProvoke(PChar) : !canCastAny(PChar, request->spells);
                  });
    std::ranges::stable_sort(capable,
                             [&](const Bot* a, const Bot* b)
                             {
                                 return preference(a->entity()->GetMJob(), a->entity()->GetMLevel(), request->kind) <
                                        preference(b->entity()->GetMJob(), b->entity()->GetMLevel(), request->kind);
                             });

    if (!capable.empty())
    {
        plans.push_back(OrderPlan{ capable.front(), order(capable.front()), fmt::format("You were just asked to {} and you're doing it now.", request->what) });
    }
    else
    {
        // Answer from a bot whose job gets it later, if there is one, with the real reason; the model fills a vague one with wrong game lore.
        auto* PAnswer = pool.front();
        for (auto* bot : pool)
        {
            if (learnLevel(bot->entity()->GetMJob(), *request) != kNever)
            {
                PAnswer = bot;
                break;
            }
        }
        plans.push_back(OrderPlan{ PAnswer, std::nullopt, cantNote(PAnswer->entity(), *request) });
    }
    return plans;
}

} // namespace bots
