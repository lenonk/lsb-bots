//
// Bot movement: player run speed, standing on the ground, and where to stand in a fight.
//

#include "bots.h"

#include "common/utils.h"

#include "map/ai/ai_container.h"
#include "map/ai/helpers/pathfind.h"
#include "map/ai/helpers/pathfind/entity_path_owner.h"
#include "map/ai/helpers/pathfind/path_owner.h"
#include "map/entities/char_entity.h"
#include "map/party.h"
#include "map/ximesh/ximesh.h"
#include "map/zone.h"

#include <cmath>
#include <numbers>

namespace bots
{

namespace
{

// The pathfinder's speed scale fits monsters; a player at the same speed value runs this much faster.
constexpr float kPlayerSpeedScale = 2.0f;

// Delegates to the normal entity owner, but reports a player's run speed.
class BotPathOwner final : public pathfind::PathOwner
{
public:
    explicit BotPathOwner(CBaseEntity* entity)
    : inner_(entity)
    {
    }

    auto position() -> position_t& override { return inner_.position(); }
    auto position() const -> const position_t& override { return inner_.position(); }
    auto navMesh() -> NavMesh& override { return inner_.navMesh(); }
    auto markPositionDirty() -> void override { inner_.markPositionDirty(); }
    auto baseSpeed() const -> uint8 override { return scaled(inner_.baseSpeed()); }
    auto updateSpeed(const bool run) -> uint8 override { return scaled(inner_.updateSpeed(run)); }
    auto isMobEntity() const -> bool override { return false; }
    auto isRoaming() const -> bool override { return inner_.isRoaming(); }
    auto inWater() const -> bool override { return inner_.inWater(); }
    auto battleTargetPosition() const -> const position_t* override { return inner_.battleTargetPosition(); }
    auto onPathPoint() -> void override {}
    auto onPathComplete() -> void override {}
    auto name() const -> const std::string& override { return inner_.name(); }
    auto id() const -> uint32 override { return inner_.id(); }
    auto hitboxRadius() const -> float override { return inner_.hitboxRadius(); }

private:
    static auto scaled(const uint8 speed) -> uint8
    {
        return static_cast<uint8>(std::min(255.0f, speed * kPlayerSpeedScale));
    }

    pathfind::EntityPathOwner inner_;
};

// Height of the collision mesh's floor under pos (y grows downward), by bisecting a downward ray.
auto groundBelow(CZone* PZone, const position_t& pos) -> float
{
    const auto* mesh = PZone->xiMesh();
    const float top  = pos.y - 1.5f;
    float       low  = top;
    float       high = pos.y + 6.0f;

    if (mesh == nullptr || !mesh->rayIntersect({ pos.x, top, pos.z }, { pos.x, high, pos.z }))
    {
        return pos.y;
    }

    for (int step = 0; step < 12; ++step)
    {
        const float mid = (low + high) / 2.0f;
        if (mesh->rayIntersect({ pos.x, top, pos.z }, { pos.x, mid, pos.z }))
        {
            high = mid;
        }
        else
        {
            low = mid;
        }
    }
    return high;
}

} // namespace

auto makePathFind(CCharEntity* PChar) -> std::unique_ptr<CPathFind>
{
    return std::make_unique<CPathFind>(std::make_unique<BotPathOwner>(PChar));
}

// Puts the bot on the ground after it moves; other clients draw it at exactly the height it's given.
void settle(Bot& bot)
{
    auto* PChar = bot.entity();
    if (PChar->loc.zone == nullptr || (PChar->loc.p.x == bot.settledAt.x && PChar->loc.p.z == bot.settledAt.z))
    {
        return;
    }

    PChar->loc.p.y = groundBelow(PChar->loc.zone, PChar->loc.p);
    bot.settledAt  = PChar->loc.p;
    PChar->updatemask |= UPDATE_POS;
}

// Where to stand against PTarget. Whoever it's attacking tanks it from the front (no spot);
// everyone else spreads over its sides and back, melee at reach and casters at range.
auto combatSpot(CCharEntity* PChar, CBattleEntity* PTarget, const float range) -> std::optional<position_t>
{
    if (PTarget->GetBattleTarget() == PChar)
    {
        return std::nullopt;
    }

    std::vector<const CBattleEntity*> flankers;
    if (PChar->PParty != nullptr)
    {
        for (const auto* PMember : PChar->PParty->members)
        {
            if (PMember->GetBattleTarget() == PTarget || PMember == PChar)
            {
                if (PTarget->GetBattleTarget() != PMember)
                {
                    flankers.push_back(PMember);
                }
            }
        }
    }
    if (flankers.empty())
    {
        flankers.push_back(PChar);
    }

    const auto index = static_cast<float>(std::ranges::find(flankers, PChar) - flankers.begin());
    const auto count = static_cast<float>(flankers.size());

    // Centred behind, about 2 yalms apart along the arc, never past the sides.
    constexpr float pi      = std::numbers::pi_v<float>;
    float           spacing = std::max(2.0f / std::max(range, 1.0f), 0.5f);
    if (count > 1.0f && spacing * (count - 1.0f) > pi)
    {
        spacing = pi / (count - 1.0f);
    }
    const float angle = pi + (index - (count - 1.0f) / 2.0f) * spacing;

    return nearPosition(PTarget->loc.p, range, angle);
}

} // namespace bots
