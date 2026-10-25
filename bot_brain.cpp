//
// The party brain: the chat model turns a party member's line into bot actions through tool calls.
//
//   One request per line, with the party roster and a fixed set of tools. The model may pick a bot
//   that can't do the job or name a spell loosely, so every call is checked here and handed to a bot
//   that can do it, or answered with why nobody can. The keyword parser in bot_orders.cpp is the
//   fallback when the model is off or doesn't answer.
//

#include "bots.h"

#include "common/logging.h"
#include "common/settings.h"

#include "map/ability.h"
#include "map/ai/ai_container.h"
#include "map/entities/char_entity.h"
#include "map/party.h"
#include "map/spell.h"
#include "map/utils/charutils.h"
#include "map/utils/zoneutils.h"
#include "map/zone.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <variant>
#include <vector>

namespace bots
{

namespace
{

using xi::Job;

struct JsonMember;

// Just enough JSON for a chat completion reply and its tool arguments.
struct Json
{
    using Array  = std::vector<Json>;
    using Object = std::vector<JsonMember>;

    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> value;

    auto get(const std::string& key) const -> const Json*;

    auto at(const std::size_t index) const -> const Json*
    {
        const auto* array = std::get_if<Array>(&value);
        return array != nullptr && index < array->size() ? &(*array)[index] : nullptr;
    }

    auto size() const -> std::size_t
    {
        const auto* array = std::get_if<Array>(&value);
        return array != nullptr ? array->size() : 0;
    }

    auto str() const -> std::string
    {
        const auto* text = std::get_if<std::string>(&value);
        return text != nullptr ? *text : std::string{};
    }
};

struct JsonMember
{
    std::string name;
    Json        value;
};

auto Json::get(const std::string& key) const -> const Json*
{
    if (const auto* object = std::get_if<Object>(&value))
    {
        for (const auto& member : *object)
        {
            if (member.name == key)
            {
                return &member.value;
            }
        }
    }
    return nullptr;
}

class JsonReader
{
public:
    explicit JsonReader(const std::string& text)
    : text_(text)
    {
    }

    auto read() -> std::optional<Json>
    {
        auto value = parse(0);
        skipSpace();
        return value && at_ == text_.size() ? value : std::nullopt;
    }

private:
    static constexpr int kMaxDepth = 32;

    void skipSpace()
    {
        while (at_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[at_])))
        {
            ++at_;
        }
    }

    auto take(const std::string_view word) -> bool
    {
        if (text_.compare(at_, word.size(), word) == 0)
        {
            at_ += word.size();
            return true;
        }
        return false;
    }

    auto parseString() -> std::optional<std::string>
    {
        if (at_ >= text_.size() || text_[at_] != '"')
        {
            return std::nullopt;
        }
        std::string out;
        for (++at_; at_ < text_.size(); ++at_)
        {
            const char c = text_[at_];
            if (c == '"')
            {
                ++at_;
                return out;
            }
            if (c != '\\')
            {
                out += c;
                continue;
            }
            if (++at_ >= text_.size())
            {
                return std::nullopt;
            }
            switch (text_[at_])
            {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b':
                case 'f': break;
                case 'u':
                {
                    // Only ASCII matters here: names, spells and keywords.
                    if (at_ + 4 >= text_.size())
                    {
                        return std::nullopt;
                    }
                    const auto code = std::stoi(text_.substr(at_ + 1, 4), nullptr, 16);
                    out += code < 0x80 ? static_cast<char>(code) : '?';
                    at_ += 4;
                    break;
                }
                default: out += text_[at_]; break;
            }
        }
        return std::nullopt;
    }

    auto parse(const int depth) -> std::optional<Json>
    {
        skipSpace();
        if (at_ >= text_.size() || depth > kMaxDepth)
        {
            return std::nullopt;
        }

        const char c = text_[at_];
        if (c == '"')
        {
            auto text = parseString();
            return text ? std::optional<Json>(Json{ std::move(*text) }) : std::nullopt;
        }
        if (c == '{')
        {
            ++at_;
            Json::Object object;
            skipSpace();
            if (at_ < text_.size() && text_[at_] == '}')
            {
                ++at_;
                return Json{ std::move(object) };
            }
            while (true)
            {
                skipSpace();
                auto key = parseString();
                skipSpace();
                if (!key || at_ >= text_.size() || text_[at_++] != ':')
                {
                    return std::nullopt;
                }
                auto member = parse(depth + 1);
                if (!member)
                {
                    return std::nullopt;
                }
                object.push_back(JsonMember{ std::move(*key), std::move(*member) });
                skipSpace();
                if (at_ < text_.size() && text_[at_] == ',')
                {
                    ++at_;
                    continue;
                }
                if (at_ < text_.size() && text_[at_] == '}')
                {
                    ++at_;
                    return Json{ std::move(object) };
                }
                return std::nullopt;
            }
        }
        if (c == '[')
        {
            ++at_;
            Json::Array array;
            skipSpace();
            if (at_ < text_.size() && text_[at_] == ']')
            {
                ++at_;
                return Json{ std::move(array) };
            }
            while (true)
            {
                auto element = parse(depth + 1);
                if (!element)
                {
                    return std::nullopt;
                }
                array.push_back(std::move(*element));
                skipSpace();
                if (at_ < text_.size() && text_[at_] == ',')
                {
                    ++at_;
                    continue;
                }
                if (at_ < text_.size() && text_[at_] == ']')
                {
                    ++at_;
                    return Json{ std::move(array) };
                }
                return std::nullopt;
            }
        }
        if (take("true"))
        {
            return Json{ true };
        }
        if (take("false"))
        {
            return Json{ false };
        }
        if (take("null"))
        {
            return Json{ nullptr };
        }

        const auto start = at_;
        while (at_ < text_.size() && (std::isdigit(static_cast<unsigned char>(text_[at_])) || std::string_view("+-.eE").find(text_[at_]) != std::string_view::npos))
        {
            ++at_;
        }
        if (start == at_)
        {
            return std::nullopt;
        }
        return Json{ std::strtod(text_.substr(start, at_ - start).c_str(), nullptr) };
    }

    const std::string& text_;
    std::size_t        at_{};
};

auto escape(const std::string& text) -> std::string
{
    std::string out;
    for (const unsigned char c : text)
    {
        switch (c)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            default: out += c < 0x20 ? std::string(" ") : std::string(1, static_cast<char>(c)); break;
        }
    }
    return out;
}

auto lowered(std::string text) -> std::string
{
    std::ranges::transform(text, text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

struct ToolCall
{
    std::string name;
    Json        args;
};

struct Job_
{
    uint64      id{};
    std::string body;
};

struct Result
{
    uint64                id{};
    bool                  answered{};
    std::vector<ToolCall> calls;
};

struct Shared
{
    std::mutex              mutex;
    std::condition_variable wake;
    std::deque<Job_>        jobs;
    std::vector<Result>     results;
};

// Never destroyed, like the chat worker's: the thread waits on it for the life of the process.
Shared& shared = *new Shared;

void worker()
{
    while (true)
    {
        Job_ job;
        {
            std::unique_lock lock(shared.mutex);
            shared.wake.wait(lock, [] { return !shared.jobs.empty(); });
            job = std::move(shared.jobs.front());
            shared.jobs.pop_front();
        }

        Result result{ .id = job.id };
        const auto reply = httpPost(settings::get<std::string>("bots.LLM_URL"), settings::get<std::string>("bots.LLM_API_KEY"), job.body,
                                    std::max(5, settings::get<int32>("bots.LLM_TIMEOUT")));
        const auto json  = reply.status == 200 ? JsonReader(reply.body).read() : std::nullopt;
        const auto* message = json && json->get("choices") && json->get("choices")->at(0) ? json->get("choices")->at(0)->get("message") : nullptr;
        if (message != nullptr)
        {
            result.answered = true;
            if (const auto* calls = message->get("tool_calls"))
            {
                for (std::size_t i = 0; i < calls->size(); ++i)
                {
                    const auto* function = calls->at(i)->get("function");
                    if (function == nullptr || function->get("name") == nullptr)
                    {
                        continue;
                    }
                    // Arguments come as a JSON string holding a JSON object.
                    const auto text = function->get("arguments") ? function->get("arguments")->str() : std::string("{}");
                    auto       args = JsonReader(text.empty() ? std::string("{}") : text).read();
                    result.calls.push_back(ToolCall{ function->get("name")->str(), args ? std::move(*args) : Json{ Json::Object{} } });
                }
            }
        }
        else
        {
            ShowWarningFmt("bots: the party brain got no usable answer (HTTP {}): {}", reply.status, reply.body.substr(0, 200));
        }

        std::lock_guard lock(shared.mutex);
        shared.results.push_back(std::move(result));
    }
}

struct Context
{
    std::string         text;
    std::string         speaker;
    std::vector<uint32> botIds;
};

// Main thread only: what each request was about.
std::map<uint64, Context> contexts;
uint64                    nextId = 1;

constexpr std::array<const char*, 23> kJobNames = {
    "",        "Warrior", "Monk",    "White Mage", "Black Mage", "Red Mage", "Thief",        "Paladin", "Dark Knight", "Beastmaster", "Bard",      "Ranger",
    "Samurai", "Ninja",   "Dragoon", "Summoner",   "Blue Mage",  "Corsair",  "Puppetmaster", "Dancer",  "Scholar",     "Geomancer",   "Rune Fencer",
};

auto jobName(const Job job) -> const char*
{
    const auto index = static_cast<std::size_t>(job);
    return index < kJobNames.size() ? kJobNames[index] : "adventurer";
}

// Spells a party might ask a bot for, listed for each bot that knows them. Nukes go in as the top tier per element.
constexpr std::array kAskable = {
    SpellID::Cure,         SpellID::Cure_II,      SpellID::Cure_III,      SpellID::Cure_IV,     SpellID::Curaga,       SpellID::Curaga_II,
    SpellID::Raise,        SpellID::Raise_II,     SpellID::Reraise,       SpellID::Poisona,     SpellID::Paralyna,     SpellID::Blindna,
    SpellID::Silena,       SpellID::Stona,        SpellID::Viruna,        SpellID::Cursna,      SpellID::Erase,        SpellID::Protect,
    SpellID::Protect_II,   SpellID::Protect_III,  SpellID::Shell,         SpellID::Shell_II,    SpellID::Shell_III,    SpellID::Protectra,
    SpellID::Shellra,      SpellID::Haste,        SpellID::Refresh,       SpellID::Regen,       SpellID::Regen_II,     SpellID::Blink,
    SpellID::Stoneskin,    SpellID::Aquaveil,     SpellID::Phalanx,       SpellID::Sneak,       SpellID::Invisible,    SpellID::Deodorize,
    SpellID::Dia,          SpellID::Dia_II,       SpellID::Dia_III,       SpellID::Bio,         SpellID::Bio_II,       SpellID::Slow,
    SpellID::Slow_II,      SpellID::Paralyze,     SpellID::Paralyze_II,   SpellID::Silence,     SpellID::Blind,        SpellID::Blind_II,
    SpellID::Bind,         SpellID::Gravity,      SpellID::Sleep,         SpellID::Sleep_II,    SpellID::Sleepga,      SpellID::Dispel,
    SpellID::Foe_Lullaby,  SpellID::Horde_Lullaby, SpellID::Battlefield_Elegy, SpellID::Carnage_Elegy, SpellID::Utsusemi_Ichi, SpellID::Utsusemi_Ni,
};

constexpr std::array<std::array<SpellID, 3>, 6> kNukeFamilies = { {
    { SpellID::Stone_III, SpellID::Stone_II, SpellID::Stone },
    { SpellID::Water_III, SpellID::Water_II, SpellID::Water },
    { SpellID::Aero_III, SpellID::Aero_II, SpellID::Aero },
    { SpellID::Fire_III, SpellID::Fire_II, SpellID::Fire },
    { SpellID::Blizzard_III, SpellID::Blizzard_II, SpellID::Blizzard },
    { SpellID::Thunder_III, SpellID::Thunder_II, SpellID::Thunder },
} };

auto spellName(const SpellID id) -> std::string
{
    auto* PSpell = spell::GetSpell(id);
    return PSpell != nullptr ? prettyName(PSpell->getName()) : std::string{};
}

// "Cure II", "cure_ii", "cure 2" and the like, to a spell id.
auto findSpell(const std::string& name) -> std::optional<SpellID>
{
    static const auto byName = []
    {
        std::map<std::string, SpellID> names;
        for (uint16 id = 1; id < MAX_SPELL_ID; ++id)
        {
            if (auto* PSpell = spell::GetSpell(static_cast<SpellID>(id)); PSpell != nullptr && PSpell->getSpellGroup() != SPELLGROUP_TRUST)
            {
                names.try_emplace(lowered(PSpell->getName()), static_cast<SpellID>(id));
            }
        }
        return names;
    }();

    std::string key;
    for (const char c : lowered(name))
    {
        if (c == ' ' || c == '-')
        {
            key += '_';
        }
        else if (c != '\'' && c != ':')
        {
            key += c;
        }
    }
    for (const auto& [digits, roman] : { std::pair{ "_4", "_iv" }, std::pair{ "_3", "_iii" }, std::pair{ "_2", "_ii" } })
    {
        if (key.ends_with(digits))
        {
            key = key.substr(0, key.size() - 2) + roman;
        }
    }
    const auto it = byName.find(key);
    return it != byName.end() ? std::optional(it->second) : std::nullopt;
}

// The asked-for spell and the lower tiers of it, best first: "Cure III" -> Cure III, Cure II, Cure.
auto ladderOf(const SpellID id) -> std::vector<SpellID>
{
    auto* PSpell = spell::GetSpell(id);
    if (PSpell == nullptr)
    {
        return {};
    }

    static constexpr std::array<std::string_view, 5> tiers = { "_v", "_iv", "_iii", "_ii", "" };

    auto base = lowered(PSpell->getName());
    for (const auto tier : tiers)
    {
        if (!tier.empty() && base.ends_with(tier))
        {
            base.resize(base.size() - tier.size());
            break;
        }
    }

    std::vector<SpellID> ladder;
    bool                 reached = false;
    for (const auto tier : tiers)
    {
        const auto next = findSpell(base + std::string(tier));
        reached         = reached || (next && *next == id);
        if (reached && next)
        {
            ladder.push_back(*next);
        }
    }
    return ladder.empty() ? std::vector<SpellID>{ id } : ladder;
}

auto canCast(CCharEntity* PChar, const std::vector<SpellID>& ladder) -> bool
{
    return std::ranges::any_of(ladder, [&](const SpellID id) { return spell::CanUseSpell(PChar, id); });
}

auto canProvoke(CCharEntity* PChar) -> bool
{
    return charutils::hasAbility(PChar, ABILITY_PROVOKE) || spell::CanUseSpell(PChar, SpellID::Flash);
}

// Healers first for help, red mages first for enfeebles, tanks first for hate.
auto castPreference(const Job job, const bool onMonster) -> int
{
    if (onMonster)
    {
        return job == Job::RDM ? 0 : job == Job::WHM ? 1 : job == Job::BLM ? 2 : job == Job::BRD ? 3 : 4;
    }
    return job == Job::WHM ? 0 : job == Job::RDM ? 1 : job == Job::SCH ? 2 : job == Job::PLD ? 3 : 4;
}

auto tankPreference(const Job job) -> int
{
    return job == Job::PLD ? 0 : job == Job::WAR ? 1 : job == Job::NIN || job == Job::RUN ? 2 : 3;
}

auto fightingName(CCharEntity* PSpeaker) -> std::string
{
    auto* PTarget = PSpeaker->GetBattleTarget();
    for (auto* PMember : PSpeaker->PParty->members)
    {
        if (PTarget == nullptr && PMember->PAI->IsEngaged())
        {
            PTarget = PMember->GetBattleTarget();
        }
    }
    return PTarget != nullptr && !PTarget->isDead() ? prettyName(PTarget->getName()) : std::string("nothing");
}

auto roster(CCharEntity* PSpeaker, const std::vector<Bot*>& bots) -> std::string
{
    std::string out = "Party (leader first):\n";
    std::vector<CBattleEntity*> members(PSpeaker->PParty->members.begin(), PSpeaker->PParty->members.end());
    std::ranges::stable_partition(members, [&](const CBattleEntity* PMember) { return PMember == PSpeaker->PParty->GetLeader(); });

    for (auto* PMember : members)
    {
        const bool isBot = std::ranges::any_of(bots, [&](const Bot* bot) { return bot->charId == PMember->id; });
        out += fmt::format("- {}: {}, {} {}{}.", PMember->getName(), isBot ? "bot" : "real player", jobName(PMember->GetMJob()), PMember->GetMLevel(),
                           PMember == PSpeaker->PParty->GetLeader() ? ", the leader" : "");
        out += PMember->isDead() ? " Dead." : fmt::format(" HP {}%{}.", PMember->GetHPP(), PMember->GetMaxMP() > 0 ? fmt::format(", MP {}%", PMember->GetMPP()) : "");

        if (auto* PChar = dynamic_cast<CCharEntity*>(PMember); isBot && PChar != nullptr)
        {
            std::vector<std::string> can{ "can pull" };
            if (canProvoke(PChar))
            {
                can.emplace_back("can Provoke");
            }
            out += " " + fmt::format("{}", fmt::join(can, ", ")) + ".";

            std::vector<std::string> spells;
            for (const auto id : kAskable)
            {
                if (spell::CanUseSpell(PChar, id))
                {
                    spells.push_back(spellName(id));
                }
            }
            for (const auto& family : kNukeFamilies)
            {
                if (const auto best = std::ranges::find_if(family, [&](const SpellID id) { return spell::CanUseSpell(PChar, id); }); best != family.end())
                {
                    spells.push_back(spellName(*best));
                }
            }
            if (!spells.empty())
            {
                out += fmt::format(" Spells: {}.", fmt::join(spells, ", "));
            }
        }
        out += "\n";
    }

    const auto camp   = campSpot(PSpeaker);
    out += fmt::format("Now: {}. {}. Fighting: {}.", PSpeaker->loc.zone ? prettyName(PSpeaker->loc.zone->getName()) : std::string("somewhere"),
                       camp ? "Camp is set" : "No camp set", fightingName(PSpeaker));
    return out;
}

constexpr std::string_view kRules =
    "You coordinate the bots in a Final Fantasy XI party. A party member just said something in party chat.\n"
    "Call exactly the tools that carry out what they asked for, or no_action.\n"
    "- Only a request or instruction gets actions. Small talk, thanks, jokes, complaints, praise (\"nice pull\") and questions\n"
    "  (\"who can raise?\") get no_action; the bots will answer those in chat.\n"
    "- One bot per job, unless the line is meant for everyone (\"everyone rest\", \"let's go\").\n"
    "- A bot named in the line gets the job if it can do it; otherwise the best bot in the roster.\n"
    "- Use only spells and abilities the roster lists for that bot.\n"
    "- Buffs and cures go on party members: \"everyone\" only when they say so, otherwise the one who spoke (\"need heals\",\n"
    "  \"cure me\"). \"That\", \"it\" or \"the add\" means a monster. Abilities like Provoke are never cast as spells.\n"
    "- Saying how they are or what they're doing is not a request: \"brb\", \"afk\", \"out of mp\", \"tired\", \"lol\". When in\n"
    "  doubt, no_action.\n"
    "- FFXI shorthand: voke = Provoke, rr = Raise, sn/inv = Sneak and Invisible, pro/shell = Protect/Shell, para = Paralyze,\n"
    "  inc = a monster is coming, add/link = an extra monster, >> points at a target, / between jobs is main/sub.\n\n";

auto tools(const std::vector<std::string>& botNames, const std::vector<std::string>& memberNames) -> std::string
{
    const auto quoted = [](const std::vector<std::string>& names)
    {
        std::vector<std::string> out;
        for (const auto& name : names)
        {
            out.push_back(fmt::format("\"{}\"", escape(name)));
        }
        return fmt::format("{}", fmt::join(out, ","));
    };
    const auto bot     = fmt::format(R"({{"type":"string","enum":[{}]}})", quoted(botNames));
    auto       targets = memberNames;
    targets.emplace_back("everyone");
    targets.emplace_back("monster");

    const auto tool = [](const std::string_view name, const std::string_view description, const std::string& properties, const std::string& required)
    {
        return fmt::format(R"({{"type":"function","function":{{"name":"{}","description":"{}","parameters":{{"type":"object","properties":{{{}}},"required":[{}]}}}}}})",
                           name, description, properties, required);
    };

    std::vector<std::string> all = {
        tool("no_action", "Nothing to do: the line isn't a request or instruction.", "", ""),
        tool("set_camp", "Make the leader's current spot the camp: the party stays there and fights monsters brought to it.", "", ""),
        tool("break_camp", "The party leaves camp and follows the leader again.", "", ""),
        tool("assign_puller", "This bot starts pulling monsters back to camp.", fmt::format(R"("bot":{})", bot), R"("bot")"),
        tool("stop_pulling", "Nobody pulls any more, or pulls pause.", "", ""),
        tool("cast", "A bot casts a spell.",
             fmt::format(R"("bot":{},"spell":{{"type":"string"}},"target":{{"type":"string","enum":[{}]}})", bot, quoted(targets)), R"("bot","spell","target")"),
        tool("provoke", "A bot provokes the monster.", fmt::format(R"("bot":{})", bot), R"("bot")"),
        tool("party_order", "An instruction for several bots at once.",
             fmt::format(R"("order":{{"type":"string","enum":["attack","stop_attacking","rest","stay","follow","assist"]}},"bots":{{"type":"array","items":{},"description":"empty means everyone"}})", bot),
             R"("order","bots")"),
    };
    return fmt::format("[{}]", fmt::join(all, ","));
}

auto findBot(const std::vector<Bot*>& bots, const std::string& name) -> Bot*
{
    const auto it = std::ranges::find_if(bots, [&](const Bot* bot) { return lowered(bot->name) == lowered(name); });
    return it != bots.end() ? *it : nullptr;
}

auto doing(const std::string& what) -> std::string
{
    return fmt::format("You were just asked to {} and you're doing it now.", what);
}

// Why nobody, or this bot, can cast it, in facts the model can repeat.
auto cantCast(CCharEntity* PChar, const SpellID id) -> std::string
{
    auto*       PSpell = spell::GetSpell(id);
    const auto  name   = spellName(id);
    const auto  level  = PSpell != nullptr ? PSpell->getJob(PChar->GetMJob()) : 255;
    if (level == 0 || level == 255)
    {
        return fmt::format("You were just asked to cast {}, but nobody in the party can: a {} never gets it. Don't claim anything about other jobs.", name,
                           jobName(PChar->GetMJob()));
    }
    if (level > PChar->GetMLevel())
    {
        return fmt::format("You were just asked to cast {}, but nobody in the party can yet: {}s get it at level {} and you're level {}.", name,
                           jobName(PChar->GetMJob()), level, PChar->GetMLevel());
    }
    return fmt::format("You were just asked to cast {}, but you can't right now (not enough MP, or it's still recharging).", name);
}

// Turns the model's calls into checked orders for bots in the party.
auto plansFor(const std::vector<ToolCall>& calls, const std::string& text, CCharEntity* PSpeaker, const std::vector<Bot*>& bots) -> std::vector<OrderPlan>
{
    std::vector<OrderPlan> plans;
    const auto order = [&](const OrderKind kind) { return Order{ .kind = kind, .speakerId = PSpeaker->id, .expires = timer::now() + std::chrono::seconds(20) }; };
    const auto everyone = [&](const OrderKind kind, const std::string& what, const std::vector<Bot*>& who)
    {
        for (std::size_t i = 0; i < who.size(); ++i)
        {
            plans.push_back(OrderPlan{ .bot = who[i], .order = order(kind), .note = doing(what), .quiet = i > 0 });
        }
    };

    for (const auto& call : calls)
    {
        const auto arg = [&](const char* key) { return call.args.get(key) ? call.args.get(key)->str() : std::string{}; };

        if (call.name == "set_camp" && !bots.empty())
        {
            plans.push_back(OrderPlan{ .bot = bots.front(), .order = order(OrderKind::Camp), .note = "The party just set up camp here, and you're staying put." });
        }
        else if (call.name == "break_camp")
        {
            const auto first = plans.size();
            everyone(OrderKind::Follow, "leave camp and follow the leader", bots);
            for (auto i = first; i < plans.size(); ++i)
            {
                plans[i].order->subject = PSpeaker->PParty->GetLeader() != nullptr ? PSpeaker->PParty->GetLeader()->id : 0;
            }
        }
        else if (call.name == "stop_pulling" && !bots.empty())
        {
            plans.push_back(OrderPlan{ .bot = bots.front(), .order = order(OrderKind::StopPull), .note = "The party just stopped having a bot pull." });
        }
        else if (call.name == "assign_puller")
        {
            if (auto* bot = findBot(bots, arg("bot")); bot != nullptr)
            {
                plans.push_back(OrderPlan{ .bot = bot, .order = order(OrderKind::Pull), .note = doing("pull monsters back to camp for the party") });
            }
        }
        else if (call.name == "provoke")
        {
            auto* bot = findBot(bots, arg("bot"));
            if (bot == nullptr || !canProvoke(bot->entity()))
            {
                std::vector<Bot*> able;
                std::ranges::copy_if(bots, std::back_inserter(able), [](Bot* other) { return canProvoke(other->entity()); });
                std::ranges::stable_sort(able, [](Bot* a, Bot* b) { return tankPreference(a->entity()->GetMJob()) < tankPreference(b->entity()->GetMJob()); });
                bot = able.empty() ? nullptr : able.front();
            }
            if (bot != nullptr)
            {
                plans.push_back(OrderPlan{ .bot = bot, .order = order(OrderKind::Provoke), .note = doing("provoke the monster") });
            }
        }
        else if (call.name == "party_order")
        {
            static const std::map<std::string, std::pair<OrderKind, const char*>> kinds = {
                { "attack", { OrderKind::Attack, "attack the monster" } }, { "stop_attacking", { OrderKind::Stop, "stop attacking" } },
                { "rest", { OrderKind::Rest, "rest" } },                   { "stay", { OrderKind::Stay, "stay where you are" } },
                { "follow", { OrderKind::Follow, "follow" } },             { "assist", { OrderKind::Assist, "assist" } },
            };
            const auto kind = kinds.find(arg("order"));
            if (kind == kinds.end())
            {
                continue;
            }
            std::vector<Bot*> who;
            if (const auto* list = call.args.get("bots"))
            {
                for (std::size_t i = 0; i < list->size(); ++i)
                {
                    if (auto* bot = findBot(bots, list->at(i)->str()); bot != nullptr)
                    {
                        who.push_back(bot);
                    }
                }
            }
            // Follow and assist: whom is read from the line (the parser gets it right more often than the model), and isn't among the bots told to.
            CBattleEntity* PSubject = nullptr;
            std::string    what     = kind->second.second;
            if (kind->second.first == OrderKind::Follow || kind->second.first == OrderKind::Assist)
            {
                PSubject = orderSubject(text, kind->second.first, PSpeaker);
                what     = fmt::format("{} {}", what, PSubject == PSpeaker ? "them" : PSubject->getName());
            }
            const auto others = [&](const std::vector<Bot*>& from)
            {
                std::vector<Bot*> out;
                std::ranges::copy_if(from, std::back_inserter(out), [&](const Bot* bot) { return PSubject == nullptr || bot->charId != PSubject->id; });
                return out;
            };
            auto doers = others(who);
            if (doers.empty())
            {
                doers = others(bots);
            }
            const auto first = plans.size();
            everyone(kind->second.first, what, doers);
            for (auto i = first; i < plans.size(); ++i)
            {
                plans[i].order->subject = PSubject != nullptr ? PSubject->id : 0;
            }
        }
        else if (call.name == "cast")
        {
            const auto spell = findSpell(arg("spell"));
            if (!spell)
            {
                ShowInfoFmt("bots: the party brain named an unknown spell: {}", arg("spell"));
                continue;
            }
            const auto ladder    = ladderOf(*spell);
            const auto target    = lowered(arg("target"));
            const bool onMonster = target == "monster";

            std::vector<uint32> targets;
            if (target == "everyone")
            {
                for (const auto* PMember : PSpeaker->PParty->members)
                {
                    targets.push_back(PMember->id);
                }
            }
            else if (!onMonster)
            {
                const auto member = std::ranges::find_if(PSpeaker->PParty->members, [&](const CBattleEntity* PMember) { return lowered(PMember->getName()) == target; });
                targets.push_back(member != PSpeaker->PParty->members.end() ? (*member)->id : PSpeaker->id);
            }

            // The named bot if it can, else the best one that can, else the reason nobody can.
            auto* bot = findBot(bots, arg("bot"));
            if (bot == nullptr || !canCast(bot->entity(), ladder))
            {
                std::vector<Bot*> able;
                std::ranges::copy_if(bots, std::back_inserter(able), [&](Bot* other) { return canCast(other->entity(), ladder); });
                std::ranges::stable_sort(able, [&](Bot* a, Bot* b) {
                    return castPreference(a->entity()->GetMJob(), onMonster) < castPreference(b->entity()->GetMJob(), onMonster);
                });
                if (able.empty())
                {
                    auto* answer = bot != nullptr ? bot : bots.empty() ? nullptr : bots.front();
                    if (answer != nullptr)
                    {
                        plans.push_back(OrderPlan{ .bot = answer, .order = std::nullopt, .note = cantCast(answer->entity(), *spell) });
                    }
                    continue;
                }
                bot = able.front();
            }

            auto cast    = order(OrderKind::Cast);
            cast.spells  = ladder;
            cast.targets = targets;
            plans.push_back(OrderPlan{ .bot = bot, .order = cast, .note = doing(fmt::format("cast {}{}", spellName(*spell), onMonster ? " on the monster" : "")) });
        }
    }
    return plans;
}

} // namespace

auto askBrain(const std::string& text, CCharEntity* PSpeaker, const std::vector<Bot*>& bots) -> uint64
{
    if (!settings::get<bool>("bots.LLM_ORDERS") || PSpeaker == nullptr || PSpeaker->PParty == nullptr || bots.empty())
    {
        return 0;
    }

    static std::once_flag started;
    std::call_once(started, [] { std::thread(worker).detach(); });

    std::vector<std::string> botNames;
    std::vector<uint32>      botIds;
    for (const auto* bot : bots)
    {
        botNames.push_back(bot->name);
        botIds.push_back(bot->charId);
    }
    std::vector<std::string> memberNames;
    for (const auto* PMember : PSpeaker->PParty->members)
    {
        memberNames.push_back(PMember->getName());
    }

    std::string model;
    if (const auto name = settings::get<std::string>("bots.LLM_MODEL"); !name.empty())
    {
        model = fmt::format(R"("model":"{}",)", escape(name));
    }

    const auto system = fmt::format("{}{}", kRules, roster(PSpeaker, bots));
    const auto body   = fmt::format(R"({{{}"messages":[{{"role":"system","content":"{}"}},{{"role":"user","content":"{}: {}"}}],"tools":{},"tool_choice":"required","max_tokens":200,"temperature":0}})",
                                    model, escape(system), escape(PSpeaker->getName()), escape(text), tools(botNames, memberNames));

    const auto id = nextId++;
    contexts[id]  = Context{ text, PSpeaker->getName(), botIds };

    std::lock_guard lock(shared.mutex);
    shared.jobs.push_back(Job_{ id, body });
    shared.wake.notify_one();
    return id;
}

auto brainOutcomes(std::vector<Bot>& online) -> std::vector<BrainOutcome>
{
    std::vector<Result> done;
    {
        std::lock_guard lock(shared.mutex);
        done.swap(shared.results);
    }

    std::vector<BrainOutcome> outcomes;
    for (auto& result : done)
    {
        const auto context = contexts.find(result.id);
        if (context == contexts.end())
        {
            continue;
        }

        BrainOutcome outcome{ .id = result.id, .answered = result.answered };
        auto*        PSpeaker = zoneutils::GetCharByName(context->second.speaker);
        if (result.answered && PSpeaker != nullptr && PSpeaker->PParty != nullptr)
        {
            // Bots that left the party or logged out since the line was said drop out.
            std::vector<Bot*> bots;
            for (const auto id : context->second.botIds)
            {
                if (const auto bot = std::ranges::find(online, id, &Bot::charId);
                    bot != online.end() && bot->entity() != nullptr && bot->entity()->PParty == PSpeaker->PParty)
                {
                    bots.push_back(&*bot);
                }
            }

            std::vector<std::string> names;
            for (const auto& call : result.calls)
            {
                names.push_back(call.name);
            }
            outcome.plans = plansFor(result.calls, context->second.text, PSpeaker, bots);
            ShowInfoFmt("bots: party brain on \"{}\": {} -> {} job(s)", context->second.text, fmt::join(names, ", "), outcome.plans.size());
        }
        contexts.erase(context);
        outcomes.push_back(std::move(outcome));
    }
    return outcomes;
}

} // namespace bots
