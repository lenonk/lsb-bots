//
// Where bots go: level-appropriate camps, and spots near real players.
//
//   A camp is the spawn point of an ordinary monster close to the bot's level,
//   in an outdoor zone or dungeon. Read from the live zones, so it follows
//   whatever content the server has enabled.
//

#include "bots.h"

#include "common/database.h"
#include "common/utils.h"
#include "common/xirand.h"

#include "map/entities/char_entity.h"
#include "map/entities/mob_entity.h"
#include "map/map_engine.h"
#include "map/navmesh/navmesh.h"
#include "map/utils/zoneutils.h"
#include "map/zone.h"

namespace bots
{

namespace
{

constexpr auto kFieldZones = static_cast<uint16>(xi::ZoneType::Outdoors) | static_cast<uint16>(xi::ZoneType::Dungeon);

auto isField(const CZone* PZone) -> bool
{
    const auto type = static_cast<uint16>(PZone->GetTypeMask());
    return (type & kFieldZones) != 0 && (type & static_cast<uint16>(xi::ZoneType::Instanced)) == 0;
}

} // namespace

auto findCamps(const uint8 level) -> std::vector<Place>
{
    std::vector<Place> camps;

    for (auto& [zoneId, PZone] : g_PZoneList)
    {
        if (PZone == nullptr || !isField(PZone))
        {
            continue;
        }

        PZone->ForEachMob(
            [&](CMobEntity* PMob)
            {
                if (PMob->m_Type == xi::MobType::Normal && PMob->m_minLevel + 1 >= level && PMob->m_maxLevel <= level + 2)
                {
                    camps.push_back(Place{ zoneId, PMob->m_SpawnPoint });
                }
            });
    }

    return camps;
}

auto campZoneNames(const uint8 level, const std::size_t count) -> std::vector<std::string>
{
    std::map<xi::ZoneId, std::size_t> spawns;
    for (const auto& camp : findCamps(level))
    {
        spawns[camp.zone]++;
    }

    std::vector<std::pair<std::size_t, xi::ZoneId>> ranked;
    for (const auto& [zone, n] : spawns)
    {
        ranked.emplace_back(n, zone);
    }
    std::ranges::sort(ranked, std::greater{});

    std::vector<std::string> names;
    for (const auto& [n, zone] : ranked)
    {
        if (names.size() >= count)
        {
            break;
        }
        if (auto* PZone = zoneutils::GetZone(zone))
        {
            names.push_back(PZone->getName());
        }
    }
    return names;
}

// A point on the navmesh between minRadius and maxRadius from around, or around itself if none turns up.
auto spotNear(const xi::ZoneId zone, const position_t& around, const float minRadius, const float maxRadius) -> position_t
{
    auto* PZone = zoneutils::GetZone(zone);
    if (PZone != nullptr && PZone->navMesh() != nullptr)
    {
        for (int attempt = 0; attempt < 8; ++attempt)
        {
            if (const auto spot = PZone->navMesh()->findRandomPosition(around, maxRadius); spot && distance(*spot, around) >= minRadius)
            {
                auto pos     = *spot;
                pos.rotation = xirand::GetRandomNumber<uint8>(255);
                return pos;
            }
        }
    }
    return around;
}

// The next login lands here; LoadChar reads the position back from the database.
void placeAt(const uint32 charId, const Place& place)
{
    db::preparedStmt("UPDATE chars SET pos_zone = ?, pos_prevzone = ?, pos_x = ?, pos_y = ?, pos_z = ?, pos_rot = ?, moghouse = 0, boundary = 0 "
                     "WHERE charid = ?",
                     place.zone, place.zone, place.pos.x, place.pos.y, place.pos.z, place.pos.rotation, charId);
}

void placeAtHome(const uint32 charId)
{
    db::preparedStmt("UPDATE chars c JOIN bot_pool b ON b.charid = c.charid "
                     "SET c.pos_zone = b.zone, c.pos_prevzone = b.zone, c.pos_x = b.pos_x, c.pos_y = b.pos_y, c.pos_z = b.pos_z, "
                     "c.pos_rot = b.pos_rot, c.moghouse = 0, c.boundary = 0 WHERE c.charid = ?",
                     charId);
}

// Zones with real players in them, each with the highest of their levels.
auto playerZones(const std::set<uint32>& botIds) -> std::map<xi::ZoneId, PlayerZone>
{
    std::map<xi::ZoneId, PlayerZone> zones;

    for (auto& [zoneId, PZone] : g_PZoneList)
    {
        if (PZone == nullptr || (static_cast<uint16>(PZone->GetTypeMask()) & static_cast<uint16>(xi::ZoneType::Instanced)) != 0)
        {
            continue;
        }

        PZone->ForEachChar(
            [&](CCharEntity* PChar)
            {
                if (botIds.contains(PChar->id) || PChar->inMogHouse() || PChar->status != xi::Status::Normal)
                {
                    return;
                }

                auto& entry = zones[zoneId];
                if (PChar->GetMLevel() >= entry.level)
                {
                    entry.level = PChar->GetMLevel();
                    entry.pos   = PChar->loc.p;
                }
            });
    }

    return zones;
}

// Zones with any real player in them, Mog Houses included; bots elsewhere go idle.
auto occupiedZones(const std::set<uint32>& botIds) -> std::set<xi::ZoneId>
{
    std::set<xi::ZoneId> zones;
    for (auto& [zoneId, PZone] : g_PZoneList)
    {
        if (PZone == nullptr)
        {
            continue;
        }

        PZone->ForEachChar(
            [&](CCharEntity* PChar)
            {
                if (!botIds.contains(PChar->id))
                {
                    zones.insert(zoneId);
                }
            });
    }
    return zones;
}

} // namespace bots
