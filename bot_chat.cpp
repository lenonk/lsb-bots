//
// Bot chat through a local LLM (llama.cpp's llama-server, OpenAI-style API).
//
//   Tells always get an answer. Party and linkshell lines get one from a bot that's
//   named, or else one bot picked at random. Say, shout and yell are answered by any
//   bot that's named; otherwise a couple of the bots that heard it may join in, and
//   the model decides from the bot's personality whether to.
//
//   Requests run on a worker thread; replies are sent from the main thread after a
//   typing delay, through the same chat packets a client sends.
//

#include "bots.h"

#include "common/database.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/xirand.h"

#include "map/ai/ai_container.h"
#include "map/entities/char_entity.h"
#include "map/entities/mob_entity.h"
#include "map/status_effect_container.h"
#include "map/enums/chat_message_type.h"
#include "map/packets/c2s/0x0b5_chat_std.h"
#include "map/packets/c2s/0x0b6_chat_name.h"
#include "map/party.h"
#include "map/utils/zoneutils.h"
#include "map/zone.h"


#include <algorithm>
#include <array>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <thread>

namespace bots
{

namespace
{

constexpr std::size_t kMemoryLines   = 12;
constexpr std::size_t kMaxPending    = 8;
constexpr std::size_t kMaxReplyChars = 110;

// Marks the bot's own lines in its memory.
constexpr std::string_view kOwnPrefix = "\x01";

struct Request
{
    uint32      charId{};
    uint8       kind{};
    std::string to; // who to answer: the sender, for tells
    bool        optional{};
    std::string body; // JSON for /v1/chat/completions
    std::string name; // the bot's, to strip it if the model signs its line
};

struct Reply
{
    uint32            charId{};
    uint8             kind{};
    std::string       to;
    std::string       text;
    timer::time_point sendAt{};
};

struct Shared
{
    std::mutex              mutex;
    std::condition_variable wake;
    std::deque<Request>     requests;
    std::vector<Reply>      replies;
};

// Never destroyed: the worker waits on it for the life of the process, and destroying a condition
// variable that still has a waiter blocks, which hung xi_map at exit.
Shared& shared = *new Shared;
std::map<uint32, std::deque<std::string>> memory; // per bot, the chat it has seen and said (main thread only)
std::map<uint32, std::string>      personalities; // main thread only
std::map<uint32, uint8>            chattiness;    // main thread only, 0-100

// Main thread only: who each player last talked with, runs of bot-only lines per channel, bots asked to be quiet.
std::map<std::string, std::pair<uint32, timer::time_point>> partners;
std::map<std::string, int>                                   botRuns;
std::map<uint32, timer::time_point>                          quietUntil;

constexpr auto kConversationWindow = std::chrono::seconds(90);
constexpr int  kMaxBotRun          = 3;
constexpr auto kQuietTime          = std::chrono::minutes(20);

auto isTell(const uint8 kind) -> bool
{
    return kind == MESSAGE_TELL;
}

auto isGroup(const uint8 kind) -> bool
{
    switch (kind)
    {
        case MESSAGE_PARTY:
        case MESSAGE_NS_PARTY:
        case MESSAGE_LINKSHELL:
        case MESSAGE_NS_LINKSHELL:
        case MESSAGE_LINKSHELL2:
        case MESSAGE_NS_LINKSHELL2:
            return true;
        default:
            return false;
    }
}

auto isParty(const uint8 kind) -> bool
{
    return kind == MESSAGE_PARTY || kind == MESSAGE_NS_PARTY;
}

auto isBroadcast(const uint8 kind) -> bool
{
    switch (kind)
    {
        case MESSAGE_SAY:
        case MESSAGE_NS_SAY:
        case MESSAGE_SHOUT:
        case MESSAGE_NS_SHOUT:
        case MESSAGE_YELL:
        case MESSAGE_UNITY:
        case MESSAGE_JP_ASSIST:
        case MESSAGE_NA_ASSIST:
            return true;
        default:
            return false;
    }
}

auto channelName(const uint8 kind) -> const char*
{
    switch (kind)
    {
        case MESSAGE_SAY:
        case MESSAGE_NS_SAY: return "say";
        case MESSAGE_SHOUT:
        case MESSAGE_NS_SHOUT: return "shout";
        case MESSAGE_TELL: return "tell";
        case MESSAGE_PARTY:
        case MESSAGE_NS_PARTY: return "party";
        case MESSAGE_LINKSHELL:
        case MESSAGE_NS_LINKSHELL:
        case MESSAGE_LINKSHELL2:
        case MESSAGE_NS_LINKSHELL2: return "linkshell";
        case MESSAGE_YELL: return "yell";
        case MESSAGE_UNITY: return "unity";
        default: return "assist";
    }
}

// The client channel a reply to this kind goes out on.
auto replyKind(const uint8 kind) -> GP_CLI_COMMAND_CHAT_STD_KIND
{
    switch (kind)
    {
        case MESSAGE_SHOUT:
        case MESSAGE_NS_SHOUT: return GP_CLI_COMMAND_CHAT_STD_KIND::Shout;
        case MESSAGE_PARTY:
        case MESSAGE_NS_PARTY: return GP_CLI_COMMAND_CHAT_STD_KIND::Party;
        case MESSAGE_LINKSHELL:
        case MESSAGE_NS_LINKSHELL:
        case MESSAGE_LINKSHELL2:
        case MESSAGE_NS_LINKSHELL2: return GP_CLI_COMMAND_CHAT_STD_KIND::Linkshell1;
        case MESSAGE_YELL: return GP_CLI_COMMAND_CHAT_STD_KIND::Yell;
        case MESSAGE_UNITY: return GP_CLI_COMMAND_CHAT_STD_KIND::Unity;
        case MESSAGE_JP_ASSIST: return GP_CLI_COMMAND_CHAT_STD_KIND::AssistJ;
        case MESSAGE_NA_ASSIST: return GP_CLI_COMMAND_CHAT_STD_KIND::AssistE;
        default: return GP_CLI_COMMAND_CHAT_STD_KIND::Say;
    }
}

auto lower(std::string text) -> std::string
{
    std::ranges::transform(text, text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Asking to group, in the words players use; the bot then sends the invite itself.
auto wantsPearl(const std::string& text) -> bool
{
    // Whole words: "ls" alone, not the end of "pls".
    std::string word;
    for (const char c : lower(text) + " ")
    {
        if (std::isalpha(static_cast<unsigned char>(c)))
        {
            word += c;
            continue;
        }
        if (word == "ls" || word == "lp" || word == "linkshell" || word == "linkpearl" || word == "pearl")
        {
            return true;
        }
        word.clear();
    }
    return false;
}

auto wantsParty(const std::string& text) -> bool
{
    static constexpr std::array<std::string_view, 11> words = { "party", "pt", "group", "grp", "invite", "inv", "join", "team", "lfp", "lfg", "duo" };
    const auto lowered = lower(text);
    return std::ranges::any_of(words,
                               [&](const std::string_view word)
                               {
                                   for (auto at = lowered.find(word); at != std::string::npos; at = lowered.find(word, at + 1))
                                   {
                                       const bool startOk = at == 0 || !std::isalpha(static_cast<unsigned char>(lowered[at - 1]));
                                       if (startOk)
                                       {
                                           return true;
                                       }
                                   }
                                   return false;
                               });
}

auto mentions(const std::string& text, const std::string& name) -> bool
{
    const auto haystack = lower(text);
    const auto needle   = lower(name);
    for (auto at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + 1))
    {
        const bool startOk = at == 0 || !std::isalpha(static_cast<unsigned char>(haystack[at - 1]));
        const bool endOk   = at + needle.size() >= haystack.size() || !std::isalpha(static_cast<unsigned char>(haystack[at + needle.size()]));
        if (startOk && endOk)
        {
            return true;
        }
    }
    return false;
}

void remember(const uint32 charId, std::string line)
{
    auto& lines = memory[charId];
    lines.push_back(std::move(line));
    while (lines.size() > kMemoryLines)
    {
        lines.pop_front();
    }
}

auto jsonEscape(const std::string& text) -> std::string
{
    std::string out;
    out.reserve(text.size() + 8);
    for (const unsigned char c : text)
    {
        switch (c)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20)
                {
                    out += fmt::format("\\u{:04x}", c);
                }
                else
                {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

// The string value after "key": in a JSON body, unescaped. Non-ASCII is dropped; the client is Shift-JIS.
auto jsonString(const std::string& body, const std::string& key, const std::size_t from = 0) -> std::optional<std::string>
{
    const auto keyAt = body.find("\"" + key + "\"", from);
    if (keyAt == std::string::npos)
    {
        return std::nullopt;
    }

    auto at = body.find('"', body.find(':', keyAt + key.size() + 2) + 1);
    if (at == std::string::npos)
    {
        return std::nullopt;
    }

    std::string out;
    for (++at; at < body.size() && body[at] != '"'; ++at)
    {
        if (body[at] != '\\')
        {
            if (static_cast<unsigned char>(body[at]) < 0x80)
            {
                out += body[at];
            }
            continue;
        }

        if (++at >= body.size())
        {
            break;
        }

        switch (body[at])
        {
            case 'n':
            case 'r':
            case 't': out += ' '; break;
            case 'u': at += 4; break; // non-ASCII escapes are dropped too
            default: out += body[at]; break;
        }
    }
    return out;
}

// One line of FFXI chat: no quotes or line breaks, nothing a chat command could act on, short enough to send.
// The FFXI client only shows ASCII: curly quotes and dashes become plain ones, emoji and the rest go.
auto toAscii(const std::string& text) -> std::string
{
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x80)
        {
            out += static_cast<char>(c);
            continue;
        }
        if (c == 0xE2 && i + 2 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0x80)
        {
            switch (static_cast<unsigned char>(text[i + 2]))
            {
                case 0x98:
                case 0x99: out += '\''; break;
                case 0x93:
                case 0x94: out += '-'; break;
                case 0xA6: out += "..."; break;
                default: break;
            }
        }
        // Skip the rest of a multi-byte character.
        while (i + 1 < text.size() && (static_cast<unsigned char>(text[i + 1]) & 0xC0) == 0x80)
        {
            ++i;
        }
    }
    return out;
}

// Stage directions: "(quietly)" and "*waves*" go, "*so*" keeps its word, and a line wrapped whole in brackets is unwrapped.
auto dropAsides(std::string text) -> std::string
{
    const auto isWords = [](const std::string& inner)
    {
        return !inner.empty() && std::ranges::all_of(inner, [](const unsigned char c) { return std::isalpha(c) || c == ' ' || c == '\'' || c == '-'; });
    };

    while (text.size() >= 2 && ((text.front() == '(' && text.back() == ')' && text.find(')') == text.size() - 1) || (text.front() == '[' && text.back() == ']' && text.find(']') == text.size() - 1)))
    {
        text = text.substr(1, text.size() - 2);
    }

    for (const auto& [open, close] : { std::pair{ '(', ')' }, std::pair{ '*', '*' } })
    {
        for (auto start = text.find(open); start != std::string::npos; start = text.find(open, start))
        {
            const auto end = text.find(close, start + 1);
            if (end == std::string::npos)
            {
                break;
            }
            const auto inner = text.substr(start + 1, end - start - 1);
            if (!isWords(inner))
            {
                start = end + 1;
                continue;
            }
            const bool oneWord = open == '*' && inner.find(' ') == std::string::npos;
            text.replace(start, end - start + 1, oneWord ? inner : std::string());
        }
    }

    std::string out;
    for (const char c : text)
    {
        if (!(c == ' ' && (out.empty() || out.back() == ' ')))
        {
            out += c;
        }
    }
    return out;
}

auto tidy(std::string text, const std::string& name) -> std::string
{
    text = toAscii(text);
    std::ranges::replace(text, '\n', ' ');
    std::erase(text, '"');

    // "Elowen: so who's pulling" - the model sometimes signs its line.
    if (const auto prefix = lower(name) + ":"; lower(text).starts_with(prefix))
    {
        text.erase(0, prefix.size());
    }
    text = dropAsides(text);

    while (!text.empty() && (std::isspace(static_cast<unsigned char>(text.front())) || text.front() == '!' || text.front() == '/'))
    {
        text.erase(text.begin());
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
    {
        text.pop_back();
    }
    if (text.size() > kMaxReplyChars)
    {
        text.resize(kMaxReplyChars);
        if (const auto space = text.rfind(' '); space != std::string::npos && space > kMaxReplyChars / 2)
        {
            text.resize(space);
        }
    }
    return text;
}

void worker()
{
    timer::time_point lastWarning{};

    while (true)
    {
        Request request;
        {
            std::unique_lock lock(shared.mutex);
            shared.wake.wait(lock, [] { return !shared.requests.empty(); });
            request = std::move(shared.requests.front());
            shared.requests.pop_front();
        }

        const auto result  = httpPost(settings::get<std::string>("bots.LLM_URL"), settings::get<std::string>("bots.LLM_API_KEY"), request.body,
                                      std::max(5, settings::get<int32>("bots.LLM_TIMEOUT")));
        const auto content = result.status == 200 ? jsonString(result.body, "content", result.body.find("\"message\"")) : std::nullopt;
        if (!content)
        {
            if (timer::now() - lastWarning > std::chrono::minutes(1))
            {
                lastWarning = timer::now();
                ShowWarningFmt("bots: no reply from the chat model at {} (HTTP {}): {}", settings::get<std::string>("bots.LLM_URL"), result.status,
                               result.body.substr(0, 300));
            }
            continue;
        }

        // The stay-quiet answer, however it's spaced or punctuated: "PASS", "pass.", or the older "(none)".
        std::string letters;
        std::ranges::copy_if(lower(*content), std::back_inserter(letters), [](const unsigned char c) { return std::isalpha(c); });
        auto text = tidy(*content, request.name);
        if (text.empty() || letters == "pass" || letters == "none")
        {
            continue;
        }

        // Roughly typing speed.
        const auto delay = std::chrono::milliseconds(std::min(4000, 400 + 35 * static_cast<int>(text.size())));

        std::lock_guard lock(shared.mutex);
        shared.replies.push_back(Reply{ request.charId, request.kind, std::move(request.to), std::move(text), timer::now() + delay });
    }
}

auto personalityOf(const Bot& bot) -> const std::string&
{
    if (const auto it = personalities.find(bot.charId); it != personalities.end())
    {
        return it->second;
    }

    std::string text;
    if (const auto rset = db::preparedStmt("SELECT personality FROM bot_pool WHERE charid = ?", bot.charId); rset && rset->next() && !rset->isNull("personality"))
    {
        text = rset->get<std::string>("personality");
    }

    // A starting personality for rows that have none; it's saved so it can be edited.
    if (text.empty())
    {
        static constexpr std::array<const char*, 10> traits = {
            "Friendly and easygoing; likes a bit of small talk.",
            "Quiet; says little, and keeps it short.",
            "Playful; teases friends and jokes now and then.",
            "Has played a long time and offers tips sometimes.",
            "Keen to level; a bit impatient with downtime.",
            "Laid back; sometimes steps away for a minute.",
            "Fairly new to the game and curious; asks questions now and then.",
            "Polite and a little formal.",
            "Dry humor; grumbles about the game but enjoys it.",
            "Proud of their nation; otherwise talks like a normal player.",
        };
        text = traits[bot.charId % traits.size()];
        db::preparedStmt("UPDATE bot_pool SET personality = ? WHERE charid = ?", text, bot.charId);
    }

    return personalities[bot.charId] = text;
}

// How often this bot joins in, 0-100; saved next to its personality so it can be edited.
auto chattinessOf(const Bot& bot) -> uint8
{
    if (const auto it = chattiness.find(bot.charId); it != chattiness.end())
    {
        return it->second;
    }

    // Matches the order of the starting personalities.
    static constexpr std::array<uint8, 10> defaults = { 80, 15, 70, 65, 40, 30, 85, 45, 55, 60 };

    uint8 value = defaults[bot.charId % defaults.size()];
    if (const auto rset = db::preparedStmt("SELECT chattiness FROM bot_pool WHERE charid = ?", bot.charId); rset && rset->next() && !rset->isNull("chattiness"))
    {
        value = std::min<uint8>(100, rset->get<uint8>("chattiness"));
    }
    else
    {
        db::preparedStmt("UPDATE bot_pool SET chattiness = ? WHERE charid = ?", value, bot.charId);
    }

    return chattiness[bot.charId] = value;
}

// Chance roll for anything optional: chiming in, answering other bots, starting conversations.
auto feelsLikeTalking(const Bot& bot) -> bool
{
    if (const auto quiet = quietUntil.find(bot.charId); quiet != quietUntil.end() && timer::now() < quiet->second)
    {
        return false;
    }
    return xirand::GetRandomNumber<uint32>(100) < chattinessOf(bot);
}

auto saysQuiet(const std::string& text) -> int
{
    const auto t = lower(text);
    for (const auto* phrase : { "you can talk", "talk again", "chat away", "you can chat", "unmute" })
    {
        if (t.find(phrase) != std::string::npos)
        {
            return -1;
        }
    }
    for (const auto* phrase : { "quiet", "shut up", "stfu", "stop talking", "less chat", "too much chat", "hush", "zip it", "pipe down", "be silent" })
    {
        if (t.find(phrase) != std::string::npos)
        {
            return 1;
        }
    }
    return 0;
}

// What's wrong with someone, as the party would see it.
auto ailments(CBattleEntity* PMember) -> std::string
{
    static constexpr std::pair<xi::StatusEffect, const char*> known[] = {
        { xi::StatusEffect::Weakness, "has Weakness from being raised or sent home, so can't fight well for a few minutes" },
        { xi::StatusEffect::SleepI, "asleep" },
        { xi::StatusEffect::SleepIi, "asleep" },
        { xi::StatusEffect::Lullaby, "asleep" },
        { xi::StatusEffect::Poison, "poisoned" },
        { xi::StatusEffect::Paralysis, "paralyzed" },
        { xi::StatusEffect::Silence, "silenced" },
        { xi::StatusEffect::Petrification, "petrified" },
        { xi::StatusEffect::Doom, "doomed" },
        { xi::StatusEffect::CurseI, "cursed" },
        { xi::StatusEffect::Blindness, "blinded" },
        { xi::StatusEffect::Slow, "slowed" },
        { xi::StatusEffect::Bind, "bound in place" },
        { xi::StatusEffect::Healing, "resting" },
    };

    std::string out;
    for (const auto& [effect, text] : known)
    {
        if (PMember->StatusEffectContainer->HasStatusEffect(effect) && out.find(text) == std::string::npos)
        {
            out += fmt::format(", {}", text);
        }
    }
    return out;
}

// Monster names close by, so small talk mentions real ones instead of invented ones.
auto nearbyMonsters(const CCharEntity* PChar) -> std::string
{
    std::set<std::string> names;
    if (PChar->loc.zone != nullptr)
    {
        PChar->loc.zone->ForEachMob(
            [&](CMobEntity* PMob)
            {
                if (names.size() < 4 && PMob->isAlive() && PMob->status != xi::Status::Disappear && distance(PChar->loc.p, PMob->loc.p) < 50.0f)
                {
                    auto name = PMob->getName();
                    std::ranges::replace(name, '_', ' ');
                    names.insert(name);
                }
            });
    }
    return names.empty() ? std::string("No monsters close by. Don't name any.")
                         : fmt::format("Monsters around you: {}. Name no others.", fmt::join(names, ", "));
}

// A line asking where to go: only then are the leveling spots worth mentioning, or the bots keep suggesting a move.
auto asksAboutPlaces(const std::string& text) -> bool
{
    const auto t = lower(text);
    return std::ranges::any_of(std::array{ "where", "place", "go after", "go next", "next spot", "spot to level", "good camp", "new camp" },
                               [&](const char* phrase) { return t.find(phrase) != std::string::npos; });
}

auto describe(const Bot& bot, const bool places = false) -> std::string
{
    static constexpr std::array<const char*, 9> races = { "", "Hume", "Hume", "Elvaan", "Elvaan", "Tarutaru", "Tarutaru", "Mithra", "Galka" };
    static constexpr std::array<const char*, 23> jobs = {
        "", "Warrior", "Monk", "White Mage", "Black Mage", "Red Mage", "Thief", "Paladin", "Dark Knight", "Beastmaster", "Bard", "Ranger",
        "Samurai", "Ninja", "Dragoon", "Summoner", "Blue Mage", "Corsair", "Puppetmaster", "Dancer", "Scholar", "Geomancer", "Rune Fencer",
    };
    static constexpr std::array<const char*, 23> shortJobs = {
        "", "WAR", "MNK", "WHM", "BLM", "RDM", "THF", "PLD", "DRK", "BST", "BRD", "RNG",
        "SAM", "NIN", "DRG", "SMN", "BLU", "COR", "PUP", "DNC", "SCH", "GEO", "RUN",
    };
    static std::map<uint8, std::string> campsByLevel;

    const auto* PChar = bot.entity();
    const auto  race  = PChar->look.race < races.size() ? races[PChar->look.race] : "Hume";
    const auto  job   = static_cast<std::size_t>(PChar->GetMJob());
    auto        zone  = PChar->loc.zone ? PChar->loc.zone->getName() : std::string("somewhere");
    std::ranges::replace(zone, '_', ' ');

    const auto spaced = [](std::string text)
    {
        std::ranges::replace(text, '_', ' ');
        return text;
    };

    // Rebuilt for every reply, so the model always sees the party and the fight as they are right now.
    std::string doing = PChar->PParty ? "in a party" : "looking for a party";
    std::string situation;
    CBattleEntity* PFight = PChar->PAI->IsEngaged() ? PChar->GetBattleTarget() : nullptr;
    if (PChar->PParty)
    {
        std::vector<std::string> members;
        for (auto* PMember : PChar->PParty->members)
        {
            const auto memberJob = static_cast<std::size_t>(PMember->GetMJob());
            auto       line      = fmt::format("{}{} {}{}", PMember == PChar ? "you, " : "", PMember->getName(), memberJob < shortJobs.size() ? shortJobs[memberJob] : "?",
                                               PMember->GetMLevel());
            if (PMember->isDead())
            {
                line += ", dead";
            }
            else
            {
                if (PMember->GetHPP() < 100 || (PMember->GetMaxMP() > 0 && PMember->GetMPP() < 100))
                {
                    line += PMember->GetMaxMP() > 0 ? fmt::format(", HP {}%, MP {}%", PMember->GetHPP(), PMember->GetMPP()) : fmt::format(", HP {}%", PMember->GetHPP());
                }
                line += ailments(PMember);
            }
            members.push_back(line);

            if (PFight == nullptr && PMember->PAI->IsEngaged() && PMember->GetBattleTarget() && PMember->GetBattleTarget()->objtype == TYPE_MOB)
            {
                PFight = PMember->GetBattleTarget();
            }
        }
        situation += fmt::format("\n- Your party right now: {}.", fmt::join(members, "; "));
    }

    if (PFight != nullptr && !PFight->isDead())
    {
        const auto* PVictim = PFight->GetBattleTarget();
        situation += fmt::format("\n- Fighting: {} at {}% HP{}.", spaced(PFight->getName()), PFight->GetHPP(),
                                 PVictim ? fmt::format(", attacking {}", PVictim == PChar ? std::string("you") : PVictim->getName()) : std::string());
    }

    auto& camps = campsByLevel[PChar->GetMLevel()];
    if (camps.empty())
    {
        auto names = campZoneNames(PChar->GetMLevel(), 3);
        for (auto& name : names)
        {
            std::ranges::replace(name, '_', ' ');
        }
        camps = names.empty() ? std::string("nowhere in particular") : fmt::format("{}", fmt::join(names, ", "));
    }

    // Only worth mentioning when it's low; otherwise the model recites it.
    std::string health;
    if (PChar->GetHPP() < 50)
    {
        health = fmt::format(" You are hurt, at {}% HP.", PChar->GetHPP());
    }
    health += situation;

    // Tested against the 4B model in ~/bg3mods/scratch/lsb-bots/chat-test.py: examples or abbreviation lists get
    // copied into every line, and without the "never invent" rule it makes up levels and numbers.
    static constexpr std::array<const char*, 3> nations = { "San d'Oria", "Bastok", "Windurst" };
    const auto nation = static_cast<std::size_t>(PChar->profile.nation);

    // Tuned on Qwen3-8B with ~/bg3mods/scratch/lsb-bots/chat-tune.py: a short prompt, no example lines (they get copied),
    // who-said-what spelled out, and the subjob stated (it made one up otherwise).
    const auto sub = PChar->GetSJob() != xi::Job::NONE && PChar->GetSLevel() > 0
                         ? fmt::format("{} subjob level {}", static_cast<std::size_t>(PChar->GetSJob()) < jobs.size() ? jobs[static_cast<std::size_t>(PChar->GetSJob())] : "?",
                                       PChar->GetSLevel())
                         : std::string("no subjob yet (they come at level 18)");
    return fmt::format("You're {0}, playing Final Fantasy XI on a small private server: {1} {2} level {3}, {11}, from {9}.\n"
                       "Right now: {4}, {5}.{6}\n"
                       "{10}{7}\n"
                       "You: {8}\n\n"
                       "The chat so far is below. Lines from other people look like \"[Party] Name: text\"; your own earlier lines are your replies.\n"
                       "Write your next line in the chat: what you'd type back to the latest line, as yourself, in under 10 words.\n"
                       "Real FFXI party chat is short and casual: lowercase, a word or two is fine, abbreviations like np, ty, lol, brb, k, wb.\n"
                       "Don't repeat or echo what they said. Plain words only: no quotes, brackets, *actions*, emoji, or your name in front.\n"
                       "Stick to what's above; don't make up items, gil, numbers, plans, places or your past. Never say you're doing something\n"
                       "unless it says so above, and never mention being an AI. If someone wants to party, you've already sent them an invite.\n"
                       "Shorthand: inc = monster coming, voke = provoke, rr = raise, sn/inv = sneak/invis, ws = weapon skill, sc = skillchain,\n"
                       "RDM/WHM = main/sub job, -,- = tired or unimpressed, >> points at a target.",
                       bot.name, race, job < jobs.size() ? jobs[job] : "adventurer", PChar->GetMLevel(), zone, doing, health,
                       places ? fmt::format("\nGood places at your level: {}.", camps) : std::string(), personalityOf(bot),
                       nation < nations.size() ? nations[nation] : "San d'Oria", nearbyMonsters(PChar), sub);
}

void request(const Bot& bot, const uint8 kind, const std::string& to, const bool optional, const std::string& system, const std::string& prompt)
{
    // The bot's own lines go back as its turns, everyone else's as the other side; the newest is the one being answered.
    std::string messages = fmt::format(R"({{"role":"system","content":"{}"}})", jsonEscape(system));
    for (const auto& line : memory[bot.charId])
    {
        const bool own  = line.starts_with(kOwnPrefix);
        messages       += fmt::format(R"(,{{"role":"{}","content":"{}"}})", own ? "assistant" : "user", jsonEscape(own ? line.substr(kOwnPrefix.size()) : line));
    }
    if (!prompt.empty())
    {
        messages += fmt::format(R"(,{{"role":"user","content":"{}"}})", jsonEscape(prompt));
    }

    // Hosted APIs need a model name; llama-server ignores it.
    std::string model;
    if (const auto name = settings::get<std::string>("bots.LLM_MODEL"); !name.empty())
    {
        model = fmt::format(R"("model":"{}",)", jsonEscape(name));
    }

    const auto body = fmt::format(R"({{{}"messages":[{}],"max_tokens":48,"temperature":0.8,"top_p":0.9,"min_p":0.05}})", model, messages);

    std::lock_guard lock(shared.mutex);
    if (optional && shared.requests.size() >= kMaxPending)
    {
        return;
    }
    shared.requests.push_back(Request{ bot.charId, kind, to, optional, body, bot.name });
    shared.wake.notify_one();
}

void ask(const Bot& bot, const Heard& heard, const bool optional, const std::string& note = {})
{
    if (optional && !feelsLikeTalking(bot))
    {
        return;
    }

    auto system = describe(bot, asksAboutPlaces(heard.text));
    if (!note.empty())
    {
        system += "\n" + note;
    }
    if (optional)
    {
        system += "\nThat wasn't said to you; chime in only if you'd naturally say something, otherwise reply PASS.";
    }
    request(bot, heard.kind, heard.from, optional, system, {});
}

// A line the bot starts itself, in the given channel.
void speakUp(const Bot& bot, const uint8 kind, const std::string& what)
{
    if (!feelsLikeTalking(bot))
    {
        return;
    }

    request(bot, kind, {}, true, fmt::format("{}\nNobody is talking right now. {}", describe(bot), what), {});
}

void say(Bot& bot, PacketSystem& packets, const Reply& reply)
{
    if (isTell(reply.kind))
    {
        auto  packet       = makePacket<GP_CLI_COMMAND_CHAT_NAME>();
        auto* tell         = packet->as<GP_CLI_COMMAND_CHAT_NAME>();
        tell->unknown04    = 3;
        std::memcpy(tell->sName, reply.to.data(), std::min(reply.to.size(), sizeof(tell->sName) - 1));
        std::memcpy(tell->Mes, reply.text.data(), std::min(reply.text.size(), sizeof(tell->Mes) - 1));
        send(bot, packets, *packet);
    }
    else
    {
        auto  packet = makePacket<GP_CLI_COMMAND_CHAT_STD>();
        auto* chat   = packet->as<GP_CLI_COMMAND_CHAT_STD>();
        chat->Kind   = static_cast<uint8_t>(replyKind(reply.kind));
        std::memcpy(chat->Str, reply.text.data(), std::min(reply.text.size(), sizeof(chat->Str) - 1));
        send(bot, packets, *packet);
    }

    remember(bot.charId, fmt::format("{}{}", kOwnPrefix, reply.text));
}

struct Listener
{
    Bot*  bot;
    Heard heard;
};

// Main thread only: party lines waiting on the brain, by request, with who heard them.
std::map<uint64, std::vector<std::pair<uint32, Heard>>> waiting;

void respond(std::vector<Listener>& listeners);
void carryOut(const std::vector<OrderPlan>& plans, const Heard& heard);

} // namespace

void startChat()
{
    if (settings::get<bool>("bots.CHAT_ENABLED"))
    {
        std::thread(worker).detach();
    }
}

// Picks who answers what, across all bots, from what each heard this tick.
void handleChat(std::vector<Bot>& online, const std::set<std::string>& botNames)
{
    if (!settings::get<bool>("bots.CHAT_ENABLED"))
    {
        for (auto& bot : online)
        {
            bot.heard.clear();
        }
        return;
    }

    std::map<std::string, std::vector<Listener>> byMessage;

    // Lines the brain has decided on: carry out the orders, or answer them like any other line.
    for (auto& outcome : brainOutcomes(online))
    {
        const auto line = waiting.find(outcome.id);
        if (line == waiting.end())
        {
            continue;
        }

        std::vector<Listener> listeners;
        for (const auto& [charId, heard] : line->second)
        {
            if (const auto bot = std::ranges::find(online, charId, &Bot::charId); bot != online.end() && bot->entity() != nullptr)
            {
                listeners.push_back(Listener{ &*bot, heard });
            }
        }
        waiting.erase(line);
        if (listeners.empty())
        {
            continue;
        }

        auto plans = outcome.plans;
        if (!outcome.answered)
        {
            std::vector<Bot*> hearing;
            for (const auto& listener : listeners)
            {
                hearing.push_back(listener.bot);
            }
            plans = planOrder(listeners.front().heard.text, zoneutils::GetCharByName(listeners.front().heard.from), hearing);
        }

        if (plans.empty())
        {
            respond(listeners);
        }
        else
        {
            carryOut(plans, listeners.front().heard);
        }
    }

    for (auto& bot : online)
    {
        for (auto& heard : bot.heard)
        {
            if (heard.from.empty() || heard.from == bot.name)
            {
                continue;
            }

            remember(bot.charId, fmt::format("[{}] {}: {}", channelName(heard.kind), heard.from, heard.text));
            byMessage[fmt::format("{}|{}|{}", heard.kind, heard.from, heard.text)].push_back(Listener{ &bot, heard });
        }
        bot.heard.clear();
    }

    for (auto& [key, listeners] : byMessage)
    {
        std::ranges::shuffle(listeners, std::mt19937{ xirand::GetRandomNumber<uint32>(std::numeric_limits<uint32>::max()) });

        const auto  kind    = listeners.front().heard.kind;
        const auto& from    = listeners.front().heard.from;
        auto*       PSender = zoneutils::GetCharByName(from);

        // Bots answer each other too, but only now and then, one at a time, and a run of bot-only lines ends
        // after a few; a real player speaking starts the count over.
        const auto channel = PSender == nullptr ? std::string{}
                             : isParty(kind)    ? fmt::format("party {}", fmt::ptr(PSender->PParty))
                             : isGroup(kind)    ? std::string("linkshell")
                                                : fmt::format("zone {}", PSender->getZone());
        if (botNames.contains(from))
        {
            auto& run = botRuns[channel];
            if (isTell(kind) || run >= kMaxBotRun)
            {
                continue;
            }
            ++run;

            const auto named = std::ranges::find_if(listeners, [&](const Listener& listener) { return mentions(listener.heard.text, listener.bot->name); });
            const auto& responder = named != listeners.end() ? *named : listeners.front();
            ask(*responder.bot, responder.heard, true);
            continue;
        }
        botRuns[channel] = 0;

        if (const auto quiet = saysQuiet(listeners.front().heard.text); quiet != 0)
        {
            for (const auto& listener : listeners)
            {
                if (quiet > 0)
                {
                    quietUntil[listener.bot->charId] = timer::now() + kQuietTime;
                }
                else
                {
                    quietUntil.erase(listener.bot->charId);
                }
            }

            quietUntil.erase(listeners.front().bot->charId);
            ask(*listeners.front().bot, listeners.front().heard, false,
                quiet > 0 ? "You were all just told to quiet down. Acknowledge in two or three words at most; you'll stay quiet after this."
                          : "You were all just told you can chat again.");
            if (quiet > 0)
            {
                quietUntil[listeners.front().bot->charId] = timer::now() + kQuietTime;
            }
            continue;
        }

        // A request from a party member ("protect please", "provoke that") is carried out, and only the bot doing it answers.
        // Party lines go to the brain and are answered once it decides; tells, and party lines when it's off, use the keyword parser.
        if (isParty(kind) || isTell(kind))
        {
            std::vector<Bot*> hearing;
            for (const auto& listener : listeners)
            {
                hearing.push_back(listener.bot);
            }

            // A tell from someone in the bot's own party is an order for that bot, read the same way.
            const bool partyTell = isTell(kind) && PSender != nullptr && PSender->PParty != nullptr &&
                                   listeners.front().bot->entity()->PParty == PSender->PParty;
            if (isParty(kind) || partyTell)
            {
                if (const auto id = askBrain(listeners.front().heard.text, PSender, hearing); id != 0)
                {
                    auto& line = waiting[id];
                    for (const auto& listener : listeners)
                    {
                        line.emplace_back(listener.bot->charId, listener.heard);
                    }
                    continue;
                }
            }

            const auto plans = planOrder(listeners.front().heard.text, PSender, hearing);
            if (!plans.empty())
            {
                carryOut(plans, listeners.front().heard);
                continue;
            }
        }

        respond(listeners);
    }
}

namespace
{

// The usual answer to a line nobody gave an order in.
void respond(std::vector<Listener>& listeners)
{
    const auto kind = listeners.front().heard.kind;

    bool named = false;
    for (const auto& listener : listeners)
    {
        if (isTell(kind) || mentions(listener.heard.text, listener.bot->name))
        {
            named = true;
            if (isTell(kind) && wantsPearl(listener.heard.text))
            {
                if (offersPearls(*listener.bot))
                {
                    listener.bot->pearlFor.push_back(listener.heard.from);
                    ask(*listener.bot, listener.heard, false, "They asked to join your linkshell. You're giving them a linkpearl now; it's in their inventory to equip.");
                }
                else
                {
                    ask(*listener.bot, listener.heard, false, "They asked about your linkshell, but you aren't in one.");
                }
                continue;
            }
            ask(*listener.bot, listener.heard, false);
            if (wantsParty(listener.heard.text))
            {
                listener.bot->invites.push_back(listener.heard.from);
            }
        }
    }

    // A follow-up ("thank you") goes to the bot the speaker was just talking to.
    const auto partner = partners.find(listeners.front().heard.from);
    const auto talking = partner == partners.end() || timer::now() - partner->second.second > kConversationWindow
                             ? listeners.end()
                             : std::ranges::find_if(listeners, [&](const Listener& listener) { return listener.bot->charId == partner->second.first; });

    // Unaddressed party chat: a question gets an answer; anything else only if a bot has something to say.
    if (isGroup(kind) && !named && talking != listeners.end())
    {
        ask(*talking->bot, talking->heard, false);
    }
    else if (isGroup(kind) && !named)
    {
        const bool question = listeners.front().heard.text.find('?') != std::string::npos;
        ask(*listeners.front().bot, listeners.front().heard, !question);
        if (listeners.size() > 1)
        {
            ask(*listeners[1].bot, listeners[1].heard, true);
        }
    }
    else if (isBroadcast(kind) && !named)
    {
        for (std::size_t i = 0; i < std::min<std::size_t>(2, listeners.size()); ++i)
        {
            ask(*listeners[i].bot, listeners[i].heard, true);
        }

        // Someone looking for a group out loud gets one invite, from a bot without a party.
        if (wantsParty(listeners.front().heard.text))
        {
            const auto free = std::ranges::find_if(listeners, [](const Listener& listener) { return listener.bot->entity()->PParty == nullptr; });
            if (free != listeners.end())
            {
                free->bot->invites.push_back(free->heard.from);
            }
        }
    }
}

// Orders from one line: a bot given more than one does them in turn, and answers once with all of them in mind.
void carryOut(const std::vector<OrderPlan>& plans, const Heard& heard)
{
    std::map<Bot*, std::string> notes;
    std::vector<Bot*>           answering;
    for (const auto& plan : plans)
    {
        if (plan.order)
        {
            if (notes.contains(plan.bot) && plan.bot->order)
            {
                plan.bot->queued.push_back(*plan.order);
            }
            else
            {
                plan.bot->order = plan.order;
                plan.bot->queued.clear();
            }
        }
        if (plan.quiet)
        {
            notes.try_emplace(plan.bot);
            continue;
        }
        auto& note = notes[plan.bot];
        if (note.empty())
        {
            answering.push_back(plan.bot);
        }
        note += (note.empty() ? "" : " ") + plan.note;
    }

    for (auto* bot : answering)
    {
        ask(*bot, heard, false, notes[bot]);
        partners[heard.from] = { bot->charId, timer::now() };
    }
}

} // namespace

// Bots that start conversations: party small talk now and then (more likely after a fight),
// and bots without a party shouting for one where real players can hear.
void chatter(std::vector<Bot>& online, const std::set<std::string>& botNames, const timer::time_point tick)
{
    if (!settings::get<bool>("bots.CHAT_ENABLED"))
    {
        return;
    }

    const auto partyMinutes = settings::get<uint32>("bots.CHAT_PARTY_MINUTES");
    const auto shoutMinutes = settings::get<uint32>("bots.CHAT_SHOUT_MINUTES");
    const auto jitter       = [](const uint32 minutes)
    {
        return std::chrono::seconds(xirand::GetRandomNumber<uint32>(minutes * 30, minutes * 90 + 1));
    };

    static std::map<const CParty*, timer::time_point> partyQuietUntil;

    // Now and then someone in the linkshell says something, while a real player is in it.
    static timer::time_point nextLinkshell{};
    if (const auto lsMinutes = settings::get<uint32>("bots.CHAT_LINKSHELL_MINUTES"); lsMinutes > 0 && tick >= nextLinkshell)
    {
        if (nextLinkshell != timer::time_point{} && !linkshellPlayers().empty())
        {
            std::vector<Bot*> wearers;
            for (auto& bot : online)
            {
                if (offersPearls(bot) && !bot.entity()->isDead())
                {
                    wearers.push_back(&bot);
                }
            }
            if (!wearers.empty())
            {
                static constexpr std::array<const char*, 8> topics = {
                    "what you're doing right now and where",
                    "a drop or some gil you just got, or one you're hoping for",
                    "asking if anyone wants to group up",
                    "a question about a quest, mission or job",
                    "a real-life aside: food, being tired, work or school tomorrow",
                    "something funny or annoying that just happened in game",
                    "levelling: what level you just hit or are close to",
                    "greeting the linkshell",
                };
                speakUp(*wearers[xirand::GetRandomNumber<std::size_t>(wearers.size())], MESSAGE_LINKSHELL,
                        fmt::format("Say something in linkshell chat about {}.", topics[xirand::GetRandomNumber<std::size_t>(topics.size())]));
            }
        }
        nextLinkshell = tick + jitter(lsMinutes);
    }

    for (auto& bot : online)
    {
        auto* PChar = bot.entity();
        if (PChar == nullptr || PChar->loc.zone == nullptr || PChar->isDead())
        {
            continue;
        }

        if (bot.nextChatter == timer::time_point{})
        {
            bot.nextChatter = tick + jitter(std::max(partyMinutes, shoutMinutes));
            continue;
        }

        if (PChar->PParty != nullptr && bot.withPlayer && partyMinutes > 0)
        {
            auto& quiet         = partyQuietUntil[PChar->PParty];
            const bool afterFight = bot.foughtAt != timer::time_point{} && tick - bot.foughtAt < std::chrono::seconds(10) && !PChar->PAI->IsEngaged();
            if (tick < quiet || (tick < bot.nextChatter && !(afterFight && xirand::GetRandomNumber<uint32>(100) < 2)))
            {
                continue;
            }

            quiet           = tick + jitter(partyMinutes) / 2;
            bot.nextChatter = tick + jitter(partyMinutes);
            static constexpr std::array<const char*, 8> topics = {
                "how the exp is going",
                "your job or subjob, or a question about someone else's",
                "gear you're saving up for",
                "where the party could go after this camp",
                "a real-life aside: food, being tired, work or school tomorrow",
                "a joke about the monsters here",
                "the pulls or the party's pace",
                "something about the party members or how the group is doing",
            };
            speakUp(bot, MESSAGE_PARTY,
                    afterFight ? std::string("You just finished a fight with your party. React to it the way a player would in party chat.")
                               : fmt::format("Start a conversation in party chat about {}.", topics[xirand::GetRandomNumber<std::size_t>(topics.size())]));
        }
        else if (PChar->PParty == nullptr && shoutMinutes > 0 && tick >= bot.nextChatter)
        {
            bot.nextChatter = tick + jitter(shoutMinutes);

            bool heard = false;
            PChar->loc.zone->ForEachChar(
                [&](CCharEntity* POther)
                {
                    heard = heard || !botNames.contains(POther->getName());
                });
            if (heard)
            {
                speakUp(bot, MESSAGE_SHOUT, "You want a party. Shout for one the way players do, mentioning your job and level.");
            }
        }
    }
}

void deliverChat(std::vector<Bot>& online, PacketSystem& packets)
{
    std::vector<Reply> due;
    {
        std::lock_guard lock(shared.mutex);
        const auto      now = timer::now();
        for (auto it = shared.replies.begin(); it != shared.replies.end();)
        {
            if (it->sendAt <= now)
            {
                due.push_back(std::move(*it));
                it = shared.replies.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    for (const auto& reply : due)
    {
        const auto bot = std::ranges::find(online, reply.charId, &Bot::charId);
        if (bot != online.end() && bot->entity() != nullptr && bot->entity()->loc.zone != nullptr)
        {
            say(*bot, packets, reply);
            if (!reply.to.empty())
            {
                partners[reply.to] = { reply.charId, timer::now() };
            }
        }
    }
}

// Party announcements, like a player's macros: raises and two-hours from everyone, weapon skills and provokes only from the talkative.
void callout(const Bot& bot, const std::string& text, const bool macro)
{
    constexpr uint8 kMacroChattiness = 40;
    if (!bot.withPlayer || (macro && chattinessOf(bot) < kMacroChattiness))
    {
        return;
    }
    if (const auto quiet = quietUntil.find(bot.charId); quiet != quietUntil.end() && timer::now() < quiet->second)
    {
        return;
    }

    std::lock_guard lock(shared.mutex);
    shared.replies.push_back({ bot.charId, MESSAGE_PARTY, {}, text, timer::now() });
}

// A hello on joining a party with a real player; not left to chattiness, but "quiet down" still holds.
void greet(const Bot& bot)
{
    if (const auto quiet = quietUntil.find(bot.charId); quiet != quietUntil.end() && timer::now() < quiet->second)
    {
        return;
    }
    request(bot, MESSAGE_PARTY, {}, false,
            fmt::format("{}\nNobody is talking right now. You just joined this party. Say hello in party chat the way a player does on joining: short, maybe your job.", describe(bot)), {});
}

// A fixed line by tell, for things that shouldn't wait on the model.
void tell(const Bot& bot, const std::string& to, const std::string& text)
{
    std::lock_guard lock(shared.mutex);
    shared.replies.push_back({ bot.charId, MESSAGE_TELL, to, text, timer::now() });
}

} // namespace bots
