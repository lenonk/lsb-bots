//
// Bots module: brings pooled bots online while real players are connected.
//
//   Settings are in settings/bots.lua, the pool in the bot_pool table.
//   Bot characters are created on startup for pool rows that don't have one yet.
//

#include "bots.h"

#include "common/database.h"
#include "common/logging.h"
#include "common/scheduler.h"
#include "common/settings.h"
#include "common/xirand.h"

#include "map/ai/ai_container.h"
#include "map/ai/helpers/pathfind.h"
#include "map/entities/char_entity.h"
#include "map/map_constants.h"
#include "map/packet_system.h"
#include "map/enums/party_kind.h"
#include "map/packets/c2s/0x00a_login.h"
#include "map/packets/c2s/0x06e_group_solicit_req.h"
#include "map/persist_batch.h"
#include "map/utils/charutils.h"
#include "map/utils/moduleutils.h"
#include "map/utils/petutils.h"
#include "map/utils/zoneutils.h"
#include "map/zone.h"

#include <algorithm>
#include <cstdlib>
#include <array>
#include <map>
#include <optional>
#include <random>
#include <set>

#include <fmt/ranges.h>

namespace
{

constexpr auto kPresenceInterval = std::chrono::seconds(5);
constexpr auto kBalanceInterval  = std::chrono::seconds(30);
constexpr auto kGroupingInterval = std::chrono::seconds(60);
constexpr auto kSlowStep         = std::chrono::milliseconds(200);
constexpr int  kNearLevel        = 6; // pool levels this close to a connected player's log in first

} // namespace

class BotModule : public CPPModule
{
    void OnInit() override
    {
        enabled_ = settings::get<bool>("bots.ENABLED");
        if (!enabled_)
        {
            ShowInfo("bots: disabled (settings/bots.lua)");
            return;
        }

        packets_ = std::make_unique<PacketSystem>();

        bots::ensureSchema();
        bots::clearStaleState();
        pool_ = bots::loadPool();
        bots::ensureLinkshell();

        for (const auto& entry : pool_)
        {
            poolIds_.insert(entry.charId);
            bots::registerPoolChar(entry.charId);
            poolNames_.insert(entry.name);
        }

        bots::startChat();

        // For the !bots and !rotatebots commands (bot_commands.lua).
        lua.set_function("BotsStatus",
                         [this]()
                         {
                             return sol::as_table(status());
                         });
        lua.set_function("BotsInspect",
                         [this](const std::string& name)
                         {
                             const auto bot = std::ranges::find_if(online_, [&](const bots::Bot& b) { return bots::lowerCase(b.name) == bots::lowerCase(name); });
                             return sol::as_table(bot != online_.end() ? bots::inspect(*bot) : std::vector<std::string>{ fmt::format("No bot called {} is online.", name) });
                         });
        lua.set_function("BotsRotate",
                         [this](const sol::optional<uint32> count)
                         {
                             return sol::as_table(rotate(count.value_or(0)));
                         });

        ShowInfoFmt("bots: {} in the pool, up to {} online", pool_.size(), settings::get<uint32>("bots.MAX_ONLINE"));
    }

    // The engine's scheduler is only reachable through a real player's session.
    void OnCharZoneIn(CCharEntity* PChar) override
    {
        if (!enabled_ || tickToken_ || poolIds_.contains(PChar->id) || !PChar->PSession || !PChar->PSession->scheduler)
        {
            return;
        }

        scheduler_ = PChar->PSession->scheduler;
        tickToken_.emplace(scheduler_->intervalOnMainThread(kLogicUpdateInterval,
                                                            [this]()
                                                            {
                                                                tick();
                                                            }));
    }

    // A real player's invite pulls the bot out of whatever party it's in, so the invite can land.
    auto OnIncomingPacket(MapSession* PSession, CCharEntity* PChar, CBasicPacket& packet) -> bool override
    {
        if (!enabled_ || packet.getType() != static_cast<uint16>(PacketC2S::GP_CLI_COMMAND_GROUP_SOLICIT_REQ) || poolIds_.contains(PChar->id))
        {
            return false;
        }

        const auto* invite = packet.as<GP_CLI_COMMAND_GROUP_SOLICIT_REQ>();
        if (invite->Kind != PartyKind::Party)
        {
            return false;
        }

        const auto it = std::ranges::find_if(online_,
                                             [&](const auto& bot)
                                             {
                                                 return bot.charId == invite->UniqueNo;
                                             });

        if (it != online_.end() && it->entity() != nullptr && it->entity()->PParty != nullptr && it->entity()->PParty != PChar->PParty)
        {
            ShowInfoFmt("bots: {} leaves its party for {}'s invite", it->name, PChar->getName());
            bots::leaveParty(*it, *packets_);
        }

        return false;
    }

    void tick()
    {
        const auto now = timer::now();

        // How long the bots take each tick, logged once a minute; the server watchdog fires at 2 s.
        struct Timing
        {
            BotModule&        module;
            timer::time_point start;
            ~Timing()
            {
                const auto took = timer::now() - start;
                module.tickCount_++;
                module.tickTotal_ += took;
                module.tickWorst_ = std::max(module.tickWorst_, took);
                if (timer::now() - module.tickReported_ >= std::chrono::minutes(1))
                {
                    const auto ms = [](const timer::duration d) { return std::chrono::duration_cast<std::chrono::microseconds>(d).count() / 1000.0; };
                    ShowInfoFmt("bots: tick {:.1f} ms average, {:.1f} ms worst over {} ticks, {} bots online", ms(module.tickTotal_ / std::max<int64>(1, module.tickCount_)),
                                ms(module.tickWorst_), module.tickCount_, module.online_.size());
                    module.tickReported_ = timer::now();
                    module.tickCount_    = 0;
                    module.tickTotal_    = {};
                    module.tickWorst_    = {};
                }
            }
        } timing{ *this, now };

        // Commands can log bots out between ticks.
        std::erase_if(online_,
                      [](const auto& bot)
                      {
                          return bot.entity() == nullptr;
                      });

        if (now >= nextPresenceCheck_)
        {
            nextPresenceCheck_ = now + kPresenceInterval;
            if (bots::countRealPlayers() > 0)
            {
                lastRealPlayer_ = now;
            }
        }

        const auto grace  = std::chrono::seconds(settings::get<uint32>("bots.LOGOUT_GRACE"));
        const bool wanted = lastRealPlayer_ != timer::time_point{} && now - lastRealPlayer_ <= kPresenceInterval + grace;
        const auto target = wanted ? std::min<std::size_t>(settings::get<uint32>("bots.MAX_ONLINE"), pool_.size()) : 0;

        if (online_.size() < target && now >= nextLogin_)
        {
            nextLogin_ = now + std::chrono::seconds(settings::get<uint32>("bots.LOGIN_INTERVAL"));
            loginNext();
        }
        else if (online_.size() > target && now >= nextLogout_)
        {
            // Over the cap with players still on: only bots outside a party go.
            nextLogout_   = now + std::chrono::seconds(1);
            const auto it = std::ranges::find_if(online_.rbegin(), online_.rend(),
                                                 [&](const auto& bot)
                                                 {
                                                     return target == 0 || isFree(bot);
                                                 });
            if (it != online_.rend())
            {
                logout(*it);
            }
        }

        if (wanted && now >= nextBalance_)
        {
            nextBalance_ = now + kBalanceInterval;
            balance();
        }

        if (wanted && now >= nextGrouping_)
        {
            nextGrouping_ = now + kGroupingInterval;
            formParties();
        }

        if (const auto minutes = settings::get<uint32>("bots.ROTATE_MINUTES"); wanted && minutes > 0 && now >= nextRotate_)
        {
            if (nextRotate_ != timer::time_point{})
            {
                rotate(1);
            }
            nextRotate_ = now + std::chrono::minutes(minutes);
        }

        const auto occupied = bots::occupiedZones(poolIds_);

        for (auto& bot : online_)
        {
            // Logged out this tick; a logout also leaves the session pending a zone, so check this first.
            if (bot.entity() == nullptr)
            {
                continue;
            }

            if (bot.session->blowfish.status == BLOWFISH_PENDING_ZONE)
            {
                if (!enterZone(bot))
                {
                    logout(bot);
                }
            }
            else if (auto* PChar = bot.entity(); PChar != nullptr)
            {
                // Anything slow enough to worry the server watchdog gets named in the log.
                const auto timed = [&](const char* what, auto&& step)
                {
                    const auto start = timer::now();
                    step();
                    if (const auto took = timer::now() - start; took > kSlowStep)
                    {
                        ShowWarningFmt("bots: {} for {} took {} ms", what, bot.name, std::chrono::duration_cast<std::chrono::milliseconds>(took).count());
                    }
                };

                if (PChar->GetMLevel() != bot.preparedLevel && !PChar->PAI->IsEngaged() && PChar->loc.zone != nullptr)
                {
                    timed("prepare", [&] { bots::prepare(bot, lua); });
                }
                timed("restock", [&] { bots::restock(bot, lua, now); });
                timed("linkshell", [&] { bots::linkshellUpkeep(bot, lua, now); });
                const bool active = bot.withPlayer || occupied.contains(PChar->getZone());
                timed("think", [&] { bots::think(bot, *packets_, poolIds_, active, now); });
                if (bot.entity() != nullptr)
                {
                    timed("settle", [&] { bots::settle(bot); });
                }
            }
        }

        bots::handleChat(online_, poolNames_);
        bots::chatter(online_, poolNames_, now);
        bots::deliverChat(online_, *packets_);

        std::erase_if(online_,
                      [](const auto& bot)
                      {
                          return bot.entity() == nullptr;
                      });
    }

    // Least-represented job first, so the online set stays mixed.
    void loginNext()
    {
        std::map<uint8, std::size_t> jobsOnline;
        for (const auto& bot : online_)
        {
            jobsOnline[bot.job]++;
        }

        std::set<uint8> playerLevels;
        for (const auto& [zone, player] : bots::playerZones(poolIds_))
        {
            playerLevels.insert(player.level);
        }
        const auto nearPlayer = [&](const bots::PoolEntry& entry)
        {
            return std::ranges::any_of(playerLevels, [&](const uint8 level) { return std::abs(static_cast<int>(entry.level) - static_cast<int>(level)) <= kNearLevel; });
        };

        std::vector<const bots::PoolEntry*> candidates;
        std::size_t                         fewest   = std::numeric_limits<std::size_t>::max();
        bool                                bestNear = false;
        for (const auto& entry : pool_)
        {
            if (!available(entry))
            {
                continue;
            }

            const bool near  = nearPlayer(entry);
            const auto count = jobsOnline[entry.job];
            if ((near && !bestNear) || (near == bestNear && count < fewest))
            {
                fewest   = count;
                bestNear = near;
                candidates.clear();
            }
            if (near == bestNear && count == fewest)
            {
                candidates.push_back(&entry);
            }
        }

        if (candidates.empty())
        {
            return;
        }

        const auto* entry = candidates[xirand::GetRandomNumber<std::size_t>(candidates.size())];

        // Most go out to a camp for their level; the rest start at their pool home.
        const auto camps = bots::findCamps(entry->level);
        if (!camps.empty() && xirand::GetRandomNumber<uint32>(100) < settings::get<uint32>("bots.CAMP_CHANCE"))
        {
            const auto& camp = camps[xirand::GetRandomNumber<std::size_t>(camps.size())];
            bots::placeAt(entry->charId, { camp.zone, bots::spotNear(camp.zone, camp.pos, 5.0f, 25.0f) });
        }
        else
        {
            bots::placeAtHome(entry->charId);
        }

        login(*entry);
    }

    // Unpartied bots in a zone with real players sometimes team up: within 5 levels, 2 to 6 of them.
    void formParties()
    {
        if (!settings::get<bool>("bots.BOT_PARTIES"))
        {
            return;
        }

        const auto occupied = bots::occupiedZones(poolIds_);
        std::map<xi::ZoneId, std::vector<bots::Bot*>> byZone;
        for (auto& bot : online_)
        {
            auto* PChar = bot.entity();
            if (PChar != nullptr && PChar->PParty == nullptr && PChar->loc.zone != nullptr && !PChar->isDead() && PChar->status == xi::Status::Normal &&
                occupied.contains(PChar->getZone()) && bot.invites.empty())
            {
                byZone[PChar->getZone()].push_back(&bot);
            }
        }

        for (auto& [zone, bots] : byZone)
        {
            if (bots.size() < 2 || xirand::GetRandomNumber<uint32>(100) >= 50)
            {
                continue;
            }

            std::ranges::sort(bots, [](const bots::Bot* a, const bots::Bot* b) { return a->entity()->GetMLevel() < b->entity()->GetMLevel(); });
            const auto start = xirand::GetRandomNumber<std::size_t>(bots.size());
            std::vector<bots::Bot*> group{ bots[start] };
            for (auto* bot : bots)
            {
                if (bot != bots[start] && group.size() < 6 &&
                    std::abs(static_cast<int>(bot->entity()->GetMLevel()) - static_cast<int>(bots[start]->entity()->GetMLevel())) <= 5)
                {
                    group.push_back(bot);
                }
            }
            if (group.size() < 2)
            {
                continue;
            }

            auto* leader = group.front();
            for (std::size_t i = 1; i < group.size(); ++i)
            {
                leader->invites.push_back(group[i]->name);
            }
            ShowInfoFmt("bots: {} is forming a party of {} in zone {}", leader->name, group.size(), zone);
        }
    }

    // Bots rotated out sit out a while, unless nobody else is left.
    auto available(const bots::PoolEntry& entry) const -> bool
    {
        if (isOnline(entry.charId))
        {
            return false;
        }

        const auto rested = resting_.find(entry.charId);
        return rested == resting_.end() || timer::now() >= rested->second || !hasUnrested();
    }

    // Keeps a few free bots in every zone a real player is in: log one in there, or bring one over from an empty zone.
    void balance()
    {
        const auto wantedPerZone = settings::get<uint32>("bots.BOTS_PER_PLAYER_ZONE");
        if (wantedPerZone == 0)
        {
            return;
        }

        const auto zones = bots::playerZones(poolIds_);

        for (const auto& [zone, players] : zones)
        {
            const auto here = std::ranges::count_if(online_,
                                                    [&](const auto& bot)
                                                    {
                                                        return bot.entity() != nullptr && !bot.withPlayer && bot.entity()->getZone() == zone;
                                                    });
            if (static_cast<uint32>(here) >= wantedPerZone)
            {
                continue;
            }

            const bots::Place place{ zone, bots::spotNear(zone, players.pos, 25.0f, 70.0f) };
            const auto        gap = [&](const uint8 level)
            {
                return std::abs(static_cast<int>(level) - static_cast<int>(players.level));
            };

            if (online_.size() < settings::get<uint32>("bots.MAX_ONLINE"))
            {
                const bots::PoolEntry* best = nullptr;
                for (const auto& entry : pool_)
                {
                    if (available(entry) && (best == nullptr || gap(entry.level) < gap(best->level)))
                    {
                        best = &entry;
                    }
                }

                if (best != nullptr)
                {
                    bots::placeAt(best->charId, place);
                    login(*best);
                    continue;
                }
            }

            bots::Bot* best = nullptr;
            for (auto& bot : online_)
            {
                auto* PChar = bot.entity();
                if (isFree(bot) && PChar->loc.zone != nullptr && PChar->status == xi::Status::Normal && !zones.contains(PChar->getZone()) &&
                    (best == nullptr || gap(PChar->GetMLevel()) < gap(best->entity()->GetMLevel())))
                {
                    best = &bot;
                }
            }

            if (best != nullptr)
            {
                ShowInfoFmt("bots: {} heads to {}", best->name, zone);
                bots::travel(best->entity(), place.zone, place.pos);
            }
        }
    }

    void login(const bots::PoolEntry& entry_)
    {
        const auto* entry = &entry_;

        bots::Bot bot{
            .charId = entry->charId,
            .accId  = entry->accId,
            .name   = entry->name,
            .job    = entry->job,
        };

        bot.session                   = std::make_unique<MapSession>();
        bot.session->scheduler        = scheduler_;
        bot.session->client_ipp       = IPP(0, static_cast<uint16>(entry->charId));
        bot.session->charID           = entry->charId;
        bot.session->accountID        = entry->accId;
        bot.session->blowfish.status  = BLOWFISH_WAITING;

        if (!enterZone(bot))
        {
            db::preparedStmt("DELETE FROM accounts_sessions WHERE charid = ?", bot.charId);
            return;
        }

        ShowInfoFmt("bots: {} logged in ({} online)", bot.name, online_.size() + 1);
        online_.push_back(std::move(bot));
    }

    // Loads the character and runs the client's zone-in, for the first login and for every zone change.
    auto enterZone(bots::Bot& bot) -> bool
    {
        auto PChar = charutils::LoadChar(bot.charId);
        if (PChar == nullptr)
        {
            ShowErrorFmt("bots: could not load {}", bot.name);
            return false;
        }

        auto* PZone = zoneutils::GetZone(PChar->loc.destination);
        if (PZone == nullptr)
        {
            ShowErrorFmt("bots: {} is in zone {}, which this map server doesn't run", bot.name, PChar->loc.destination);
            return false;
        }

        const bool loggingIn = bot.session->blowfish.status == BLOWFISH_WAITING;
        if (loggingIn)
        {
            std::array<uint8, 20> key{};
            for (auto& byte : key)
            {
                byte = xirand::GetRandomNumber<uint8>(255);
            }

            db::preparedStmt("DELETE FROM accounts_sessions WHERE charid = ?", bot.charId);
            if (!db::preparedStmt("INSERT INTO accounts_sessions (accid, charid, session_key, server_addr, server_port, client_addr, client_port) "
                                  "VALUES (?, ?, ?, ?, ?, 0, ?)",
                                  bot.accId, bot.charId, key, PZone->GetIP(), PZone->GetPort(), bot.session->client_ipp.getPort()))
            {
                ShowErrorFmt("bots: could not create a session for {}", bot.name);
                return false;
            }
        }

        PChar->PSession       = bot.session.get();
        PChar->PAI->PathFind  = bots::makePathFind(PChar.get());
        bot.session->PChar    = std::move(PChar);

        auto  packet          = bots::makePacket<GP_CLI_COMMAND_LOGIN>();
        packet->as<GP_CLI_COMMAND_LOGIN>()->UniqueNo = bot.charId;
        bots::send(bot, *packets_, *packet);

        auto* PEntity = bot.entity();
        if (PEntity->loc.zone == nullptr)
        {
            ShowErrorFmt("bots: {} did not get into zone {}", bot.name, PEntity->loc.destination);
            return false;
        }

        bot.home = PEntity->loc.p;

        if (loggingIn)
        {
            bots::prepare(bot, lua);
        }

        // Rested while offline; also covers characters created at a level above their stored HP.
        if (loggingIn && !PEntity->isDead())
        {
            PEntity->addHP(PEntity->GetMaxHP());
            PEntity->addMP(PEntity->GetMaxMP());
        }

        return true;
    }

    // Mirrors the session cleanup a timed-out client gets.
    void logout(bots::Bot& bot)
    {
        if (auto* PChar = bot.entity(); PChar != nullptr && PChar->loc.zone != nullptr)
        {
            // Bots level in parties; the next placement goes by the new level.
            if (const auto entry = std::ranges::find(pool_, bot.charId, &bots::PoolEntry::charId); entry != pool_.end())
            {
                entry->level = PChar->GetMLevel();
            }

            persist::flush(PChar, IsLogout::Yes);
            charutils::SaveCharPosition(PChar);

            if (PChar->PPet != nullptr && PChar->PPet->objtype == TYPE_MOB)
            {
                petutils::DespawnPet(PChar);
            }

            PChar->status = xi::Status::Shutdown;
            charutils::removeCharFromZone(PChar);
        }

        db::preparedStmt("DELETE FROM accounts_sessions WHERE charid = ?", bot.charId);
        bot.session->PChar.reset();

        ShowInfoFmt("bots: {} logged out ({} online)", bot.name, online_.size() - 1);
    }

    static auto isFree(const bots::Bot& bot) -> bool
    {
        const auto* PChar = bot.entity();
        return PChar != nullptr && PChar->PParty == nullptr;
    }

    auto isOnline(const uint32 charId) const -> bool
    {
        return std::ranges::any_of(online_,
                                   [&](const auto& bot)
                                   {
                                       return bot.charId == charId && bot.entity() != nullptr;
                                   });
    }

    auto hasUnrested() const -> bool
    {
        const auto now = timer::now();
        return std::ranges::any_of(pool_,
                                   [&](const auto& entry)
                                   {
                                       const auto rested = resting_.find(entry.charId);
                                       return !isOnline(entry.charId) && (rested == resting_.end() || now >= rested->second);
                                   });
    }

    // Logs out up to count bots that aren't in a party (0: all of them); replacements follow at the login interval.
    auto rotate(const uint32 count) -> std::vector<std::string>
    {
        std::vector<bots::Bot*> free;
        for (auto& bot : online_)
        {
            if (isFree(bot))
            {
                free.push_back(&bot);
            }
        }

        std::ranges::shuffle(free, std::mt19937{ xirand::GetRandomNumber<uint32>(std::numeric_limits<uint32>::max()) });
        if (count > 0 && free.size() > count)
        {
            free.resize(count);
        }

        std::vector<std::string> names;
        for (auto* bot : free)
        {
            names.push_back(bot->name);
            resting_[bot->charId] = timer::now() + std::chrono::minutes(15);
            logout(*bot);
        }

        nextLogin_ = timer::now();
        if (names.empty())
        {
            return { "No bots outside a party to rotate." };
        }

        std::vector<std::string> lines{ fmt::format("Rotating out {} bots; others will log in to replace them:", names.size()) };
        for (std::size_t i = 0; i < names.size(); i += 5)
        {
            const auto end = std::min(names.size(), i + 5);
            lines.push_back(fmt::format("  {}", fmt::join(names.begin() + i, names.begin() + end, ", ")));
        }
        return lines;
    }

    // One line per bot, so the chat log doesn't cut it off.
    auto status() const -> std::vector<std::string>
    {
        static constexpr std::array<const char*, 23> jobs = {
            "",    "WAR", "MNK", "WHM", "BLM", "RDM", "THF", "PLD", "DRK", "BST", "BRD", "RNG",
            "SAM", "NIN", "DRG", "SMN", "BLU", "COR", "PUP", "DNC", "SCH", "GEO", "RUN",
        };

        std::vector<std::string> lines;
        for (const auto& bot : online_)
        {
            const auto* PChar = bot.entity();
            if (PChar == nullptr || PChar->loc.zone == nullptr)
            {
                continue;
            }

            auto zone = PChar->loc.zone->getName();
            std::ranges::replace(zone, '_', ' ');

            const auto job = static_cast<std::size_t>(PChar->GetMJob());
            lines.push_back(fmt::format("  {} - {}{} - {}{}", bot.name, job < jobs.size() ? jobs[job] : "?", PChar->GetMLevel(), zone,
                                        PChar->PParty ? " (in a party)" : ""));
        }

        lines.insert(lines.begin(), fmt::format("Bots online: {} of {}", lines.size(), pool_.size()));
        return lines;
    }

    int64                         tickCount_{};
    timer::duration               tickTotal_{};
    timer::duration               tickWorst_{};
    timer::time_point             tickReported_{};
    bool                          enabled_{ false };
    Scheduler*                    scheduler_{ nullptr };
    std::optional<Scheduler::Token> tickToken_;
    std::unique_ptr<PacketSystem> packets_; // its rate limiter reads Lua settings, so not at static init
    std::vector<bots::PoolEntry>  pool_;
    std::set<uint32>              poolIds_;
    std::set<std::string>         poolNames_;
    std::vector<bots::Bot>        online_;
    timer::time_point             nextPresenceCheck_{};
    timer::time_point             lastRealPlayer_{};
    timer::time_point             nextLogin_{};
    timer::time_point             nextLogout_{};
    timer::time_point             nextRotate_{};
    timer::time_point             nextBalance_{};
    timer::time_point             nextGrouping_{};
    std::map<uint32, timer::time_point> resting_; // rotated-out bots and when they may return
};

REGISTER_CPP_MODULE(BotModule);
