//
// Bot preparation: capped skills, every spell the job knows at its level, and gear.
//
//   Run at login and whenever the bot's level changes. Gear is the highest-level
//   item each slot can take for the job and race; weapons by damage per delay.
//

#include "bots.h"

#include "common/database.h"
#include "common/logging.h"
#include "common/settings.h"

#include "map/ability.h"
#include "map/blue_spell.h"
#include "map/entities/char_entity.h"
#include "map/items.h"
#include "map/items/item_equipment.h"
#include "map/items/item_weapon.h"
#include "map/spell.h"
#include "map/utils/blueutils.h"
#include "map/utils/charutils.h"
#include "map/utils/itemutils.h"

#include <array>
#include <vector>

namespace bots
{

namespace
{

using xi::SkillType;

constexpr int32 kToolStock     = 10; // ninja tools kept on hand
constexpr int32 kMedicineStock = 3;  // Echo Drops, Silent Oil and Prism Powder
constexpr uint16 kAnimator     = 17859;
constexpr uint16 kAutomatonOil = 18731; // Repair takes it from the ammo slot

// Blue magic worth setting, most wanted first, with the level it comes at.
constexpr std::array<std::pair<SpellID, uint8>, 19> kBlueSpells = { {
    { SpellID::Pollen, 1 },         { SpellID::Power_Attack, 4 },   { SpellID::Cocoon, 8 },          { SpellID::Metallic_Body, 8 },
    { SpellID::Head_Butt, 12 },     { SpellID::Healing_Breeze, 16 }, { SpellID::Bludgeon, 18 },      { SpellID::Claw_Cyclone, 20 },
    { SpellID::Screwdriver, 26 },   { SpellID::Wild_Carrot, 30 },   { SpellID::Sound_Blast, 32 },    { SpellID::Death_Ray, 34 },
    { SpellID::Jet_Stream, 38 },    { SpellID::Sickle_Slash, 48 },  { SpellID::Refueling, 48 },      { SpellID::Magic_Fruit, 58 },
    { SpellID::Hysteric_Barrage, 69 }, { SpellID::Disseverment, 72 }, { SpellID::Sheep_Song, 16 },
} };

// Rolls a Corsair would have bought the dice for.
constexpr std::array<uint16, 8> kRolls = { ABILITY_CORSAIRS_ROLL, ABILITY_HUNTERS_ROLL, ABILITY_CHAOS_ROLL, ABILITY_HEALERS_ROLL,
                                           ABILITY_SAMURAI_ROLL,  ABILITY_EVOKERS_ROLL, ABILITY_WARLOCKS_ROLL, ABILITY_FIGHTERS_ROLL };

// One full stack: 99 arrows, 12 jugs.
auto stackOf(const uint16 id) -> int32
{
    const auto* PItem = xi::items::lookup<CItem>(id);
    return PItem != nullptr ? std::max<int32>(1, PItem->getStackSize()) : 1;
}

// Weapon skills each job fights with, best first.
auto preferredSkills(const xi::Job job) -> std::vector<SkillType>
{
    switch (job)
    {
        case xi::Job::WAR: return { SkillType::GreatAxe, SkillType::Axe };
        case xi::Job::MNK: return { SkillType::HandToHand };
        case xi::Job::WHM: return { SkillType::Club };
        case xi::Job::BLM: return { SkillType::Staff };
        case xi::Job::RDM: return { SkillType::Sword };
        case xi::Job::THF: return { SkillType::Dagger };
        case xi::Job::PLD: return { SkillType::Sword };
        case xi::Job::DRK: return { SkillType::Scythe, SkillType::GreatSword };
        case xi::Job::BST: return { SkillType::Axe };
        case xi::Job::BRD: return { SkillType::Dagger };
        case xi::Job::RNG: return { SkillType::Dagger, SkillType::Axe };
        case xi::Job::SAM: return { SkillType::GreatKatana };
        case xi::Job::NIN: return { SkillType::Katana };
        case xi::Job::DRG: return { SkillType::Polearm };
        case xi::Job::SMN: return { SkillType::Staff };
        case xi::Job::BLU: return { SkillType::Sword };
        case xi::Job::COR: return { SkillType::Dagger };
        case xi::Job::PUP: return { SkillType::HandToHand };
        case xi::Job::DNC: return { SkillType::Dagger };
        case xi::Job::SCH: return { SkillType::Staff };
        case xi::Job::GEO: return { SkillType::Club };
        case xi::Job::RUN: return { SkillType::GreatSword };
        default: return {};
    }
}

// Ranged weapon skills, best first; only jobs that fight with one.
auto rangedSkills(const xi::Job job) -> std::vector<SkillType>
{
    switch (job)
    {
        case xi::Job::RNG: return { SkillType::Archery, SkillType::Marksmanship };
        case xi::Job::COR: return { SkillType::Marksmanship };
        default: return {};
    }
}

auto usable(const CItemEquipment* PItem, const xi::Job job, const uint8 level, const uint8 race) -> bool
{
    // GmOnly is GM gear like the Judge's set; players can't get it.
    return !PItem->hasFlag(xi::ItemFlag::GmOnly) && PItem->getReqLvl() <= level && PItem->getILvl() == 0 && PItem->getRemoveSlotId() == 0 &&
           (PItem->getJobs() & (1u << (static_cast<uint8>(job) - 1))) != 0 && PItem->isEquippableByRace(race);
}

auto armourScore(const CItemEquipment* PItem) -> int32
{
    return PItem->getReqLvl() * 1000 + PItem->getModifier(xi::Mod::DEF) * 10 + PItem->getModifier(xi::Mod::HP) + PItem->getModifier(xi::Mod::MP);
}

// Every piece of equipment, gathered once rather than scanning every item id at each level-up.
auto allEquipment() -> const std::vector<std::pair<uint16, const CItemEquipment*>>&
{
    static const auto items = []
    {
        std::vector<std::pair<uint16, const CItemEquipment*>> found;
        for (uint16 id = 1; id < MAX_ITEMID; ++id)
        {
            if (const auto* PItem = xi::items::lookup<CItemEquipment>(id))
            {
                found.emplace_back(id, PItem);
            }
        }
        return found;
    }();
    return items;
}

// Item id per equipment slot, 0 for none.
auto planGear(const xi::Job job, const uint8 level, const uint8 race) -> std::array<uint16, 16>
{
    std::array<uint16, 16> plan{};
    std::array<int32, 16>  best{};
    best.fill(-1);

    // Rings and earrings take the best two distinct items.
    std::vector<std::pair<int32, uint16>> rings;
    std::vector<std::pair<int32, uint16>> earrings;

    const auto skills      = preferredSkills(job);
    int32      bestWeapon  = -1;
    bool       twoHanded   = false;
    std::size_t bestSkill  = skills.size();

    const auto  ranged          = rangedSkills(job);
    int32       bestRanged      = -1;
    std::size_t bestRangedSkill = ranged.size();
    std::vector<const CItemWeapon*> ammo;

    for (const auto& [id, PItem] : allEquipment())
    {
        if (!usable(PItem, job, level, race))
        {
            continue;
        }

        const auto slots = PItem->getEquipSlotId();

        if (const auto* PWeapon = dynamic_cast<const CItemWeapon*>(PItem); PWeapon && (slots & (1 << SLOT_MAIN)))
        {
            const auto rank = static_cast<std::size_t>(std::ranges::find(skills, PWeapon->getSkillType()) - skills.begin());
            if (rank == skills.size() || PWeapon->getDelay() == 0)
            {
                continue;
            }

            const auto score = static_cast<int32>(PWeapon->getDamage() * 100000 / PWeapon->getDelay());
            if (rank < bestSkill || (rank == bestSkill && score > bestWeapon))
            {
                bestSkill          = rank;
                bestWeapon         = score;
                plan[SLOT_MAIN]    = id;
                twoHanded          = PWeapon->isTwoHanded();
            }
            continue;
        }

        if (const auto* PWeapon = dynamic_cast<const CItemWeapon*>(PItem); PWeapon && (slots & (1 << SLOT_RANGED)))
        {
            const auto rank = static_cast<std::size_t>(std::ranges::find(ranged, PWeapon->getSkillType()) - ranged.begin());
            const auto score = PWeapon->getDelay() > 0 ? static_cast<int32>(PWeapon->getDamage() * 100000 / PWeapon->getDelay()) : -1;
            if (rank < ranged.size() && score >= 0 && (rank < bestRangedSkill || (rank == bestRangedSkill && score > bestRanged)))
            {
                bestRangedSkill   = rank;
                bestRanged        = score;
                plan[SLOT_RANGED] = id;
            }
            continue;
        }

        if (const auto* PWeapon = dynamic_cast<const CItemWeapon*>(PItem); PWeapon && (slots & (1 << SLOT_AMMO)))
        {
            // Only ammunition that comes in stacks, so it can be restocked.
            if ((!ranged.empty() || job == xi::Job::BST) && PWeapon->getStackSize() > 1)
            {
                ammo.push_back(PWeapon);
            }
            continue;
        }

        if (slots & (1 << SLOT_SUB))
        {
            if (PItem->IsShield() && armourScore(PItem) > best[SLOT_SUB])
            {
                best[SLOT_SUB] = armourScore(PItem);
                plan[SLOT_SUB] = id;
            }
            continue;
        }

        if (slots & (1 << SLOT_RING1))
        {
            rings.emplace_back(armourScore(PItem), id);
            continue;
        }

        if (slots & (1 << SLOT_EAR1))
        {
            earrings.emplace_back(armourScore(PItem), id);
            continue;
        }

        for (const auto slot : { SLOT_HEAD, SLOT_BODY, SLOT_HANDS, SLOT_LEGS, SLOT_FEET, SLOT_NECK, SLOT_WAIST, SLOT_BACK })
        {
            if ((slots & (1 << slot)) && armourScore(PItem) > best[slot])
            {
                best[slot] = armourScore(PItem);
                plan[slot] = id;
            }
        }
    }

    if (twoHanded || plan[SLOT_MAIN] == 0)
    {
        plan[SLOT_SUB] = 0;
    }

    // Ammunition the ranged weapon fires (bows of either kind share arrows), hardest hitting first.
    if (const auto* PRanged = xi::items::lookup<CItemWeapon>(plan[SLOT_RANGED]))
    {
        int32 bestAmmo = -1;
        for (const auto* PAmmo : ammo)
        {
            const bool fits = PAmmo->getSkillType() == PRanged->getSkillType() &&
                              (PRanged->getSkillType() == SkillType::Archery || PAmmo->getSubSkillType() == PRanged->getSubSkillType());
            if (fits && PAmmo->getDamage() > bestAmmo)
            {
                bestAmmo        = PAmmo->getDamage();
                plan[SLOT_AMMO] = PAmmo->getID();
            }
        }
        if (plan[SLOT_AMMO] == 0)
        {
            plan[SLOT_RANGED] = 0;
        }
    }

    // A beastmaster's ammo slot holds the jug its pet comes from: the highest level one.
    if (job == xi::Job::BST)
    {
        uint8 bestJug = 0;
        for (const auto* PJug : ammo)
        {
            if (PJug->getReqLvl() > bestJug)
            {
                bestJug         = PJug->getReqLvl();
                plan[SLOT_AMMO] = PJug->getID();
            }
        }
    }

    const auto takeTwo = [&](auto& items, const SLOTTYPE first, const SLOTTYPE second)
    {
        std::ranges::sort(items, std::greater{});
        if (!items.empty())
        {
            plan[first] = items[0].second;
        }
        if (items.size() > 1)
        {
            plan[second] = items[1].second;
        }
    };
    takeTwo(rings, SLOT_RING1, SLOT_RING2);
    takeTwo(earrings, SLOT_EAR1, SLOT_EAR2);

    return plan;
}

// Main and sub job spells, at each job's level.
void learnSpells(CCharEntity* PChar)
{
    const auto known = [&](const xi::Job job, const uint8 level, CSpell* PSpell)
    {
        const auto required = PSpell->getJob(job);
        return job != xi::Job::NONE && required > 0 && required <= level;
    };

    for (uint16 id = 1; id < MAX_SPELL_ID; ++id)
    {
        auto* PSpell = spell::GetSpell(static_cast<SpellID>(id));
        if (PSpell == nullptr || PSpell->getSpellGroup() == SPELLGROUP_TRUST || PSpell->getSpellGroup() == SPELLGROUP_BLUE)
        {
            continue;
        }

        if ((known(PChar->GetMJob(), PChar->GetMLevel(), PSpell) || known(PChar->GetSJob(), PChar->GetSLevel(), PSpell)) && !charutils::hasSpell(PChar, id))
        {
            charutils::addSpell(PChar, id);
            charutils::SaveSpell(PChar, id);
        }
    }
}

// A Blue Mage learns the spells on its list up to its level and sets as many as its slots and points allow, in list order.
void setBlueMagic(CCharEntity* PChar)
{
    if (PChar->GetMJob() != xi::Job::BLU)
    {
        return;
    }

    for (const auto& [id, level] : kBlueSpells)
    {
        auto* PSpell = dynamic_cast<CBlueSpell*>(spell::GetSpell(id));
        if (PSpell == nullptr || level > PChar->GetMLevel())
        {
            continue;
        }
        if (!charutils::hasSpell(PChar, static_cast<uint16>(id)))
        {
            charutils::addSpell(PChar, static_cast<uint16>(id));
            charutils::SaveSpell(PChar, static_cast<uint16>(id));
        }
        if (blueutils::IsSpellSet(PChar, PSpell))
        {
            continue;
        }
        for (uint8 slot = 0; slot < blueutils::GetTotalSlots(PChar); ++slot)
        {
            if (PChar->m_SetBlueSpells[slot] == 0)
            {
                if (blueutils::HasEnoughSetPoints(PChar, PSpell, slot))
                {
                    blueutils::SetBlueSpell(PChar, PSpell, slot, true);
                }
                break;
            }
        }
    }
    blueutils::CalculateTraits(PChar);
    charutils::BuildingCharTraitsTable(PChar);
}

// The usual early pairing for each job, all from the six starting jobs so no advanced-job unlock is needed.
auto defaultSubJob(const xi::Job job) -> xi::Job
{
    switch (job)
    {
        case xi::Job::WAR: return xi::Job::MNK;
        case xi::Job::WHM: return xi::Job::BLM;
        case xi::Job::BLM:
        case xi::Job::RDM:
        case xi::Job::BST:
        case xi::Job::BRD:
        case xi::Job::SMN:
        case xi::Job::GEO: return xi::Job::WHM;
        case xi::Job::SCH: return xi::Job::RDM;
        default: return xi::Job::WAR;
    }
}

// From the subjob quest level on, a subjob at half the main level; bot_pool.subjob overrides the pairing.
auto subJobScript(const Bot& bot, CCharEntity* PChar) -> std::string
{
    const auto unlockLevel = settings::get<uint8>("main.SUBJOB_QUEST_LEVEL");
    if (PChar->GetMLevel() < unlockLevel)
    {
        return {};
    }

    auto sub = defaultSubJob(PChar->GetMJob());
    if (const auto rset = db::preparedStmt("SELECT subjob FROM bot_pool WHERE charid = ?", bot.charId); rset && rset->next() && !rset->isNull("subjob"))
    {
        sub = static_cast<xi::Job>(rset->get<uint8>("subjob"));
    }
    if (sub == PChar->GetMJob() || sub == xi::Job::NONE)
    {
        return {};
    }

    // The sub job's own level is set to the main level; the game runs it at half that.
    return fmt::format("p:unlockJob(0)\nif p:getSubJob() ~= {0} then p:changesJob({0}) end\np:setsLevel({1})\n", static_cast<uint8>(sub), PChar->GetMLevel());
}

} // namespace

void prepare(Bot& bot, sol::state& lua)
{
    auto* PChar = bot.entity();
    bot.preparedLevel = PChar->GetMLevel();

    if (const auto subJob = subJobScript(bot, PChar); !subJob.empty())
    {
        const auto script = fmt::format("local p = GetPlayerByID({})\nif p == nil then return end\n{}", PChar->id, subJob);
        if (const auto result = lua.safe_script(script, sol::script_pass_on_error); !result.valid())
        {
            const sol::error err = result;
            ShowErrorFmt("bots: setting a subjob for {} failed: {}", bot.name, err.what());
        }
    }

    learnSpells(PChar);
    setBlueMagic(PChar);

    // A Puppetmaster's maneuvers need an Animator, and Repair takes oil from the ammo slot.
    auto plan = planGear(PChar->GetMJob(), PChar->GetMLevel(), PChar->look.race);
    if (PChar->GetMJob() == xi::Job::PUP)
    {
        plan[SLOT_RANGED] = kAnimator;
        plan[SLOT_AMMO]   = kAutomatonOil;
    }
    bot.ammoId = plan[SLOT_AMMO];

    // Items go through the Lua API, the same path GM commands use.
    std::string script = fmt::format("local p = GetPlayerByID({})\nif p == nil then return end\np:capAllSkills()\np:setNewPlayer(false)\n", PChar->id);

    // The dice for each roll; the game hands out a roll only once its level comes.
    if (PChar->GetMJob() == xi::Job::COR || PChar->GetSJob() == xi::Job::COR)
    {
        for (const auto roll : kRolls)
        {
            script += fmt::format("if not p:hasLearnedAbility({0}) then p:addLearnedAbility({0}) end\n", roll);
        }
    }
    for (uint8 slot = 0; slot < plan.size(); ++slot)
    {
        if (plan[slot] == 0)
        {
            // Nothing better for the slot, but GM gear from before the GmOnly check still goes.
            if (const auto* PWorn = PChar->getEquip(static_cast<SLOTTYPE>(slot)); PWorn != nullptr && PWorn->hasFlag(xi::ItemFlag::GmOnly))
            {
                script += fmt::format("p:unequipItem({}); p:delItem({}, 1)\n", slot, PWorn->getID());
            }
            continue;
        }

        script += fmt::format("do local old = p:getEquipID({0}); if old ~= {1} then "
                              "if not p:hasItem({1}) then p:addItem({{ id = {1}, quantity = {2}, silent = true }}) end; p:equipItem({1}, nil, {0}); "
                              "if old ~= 0 and p:getEquipID({0}) == {1} then p:delItem(old, 1) end end end\n",
                              slot, plan[slot], slot == SLOT_AMMO ? stackOf(plan[slot]) : 1);
    }

    if (const auto result = lua.safe_script(script, sol::script_pass_on_error); !result.valid())
    {
        const sol::error err = result;
        ShowErrorFmt("bots: preparing {} failed: {}", bot.name, err.what());
    }
}

// Ammunition back in the slot once a stack runs out, and ninja tools for Utsusemi.
void restock(Bot& bot, sol::state& lua, const timer::time_point tick)
{
    auto* PChar = bot.entity();
    if (PChar == nullptr || !throttle(bot, 0x30000, 0, std::chrono::seconds(20), tick))
    {
        return;
    }

    std::string script;
    if (bot.ammoId != 0 && PChar->getEquip(SLOT_AMMO) == nullptr)
    {
        script += fmt::format("if not p:hasItem({0}) then p:addItem({{ id = {0}, quantity = {1}, silent = true }}) end\np:equipItem({0}, nil, {2})\n",
                              bot.ammoId, stackOf(bot.ammoId), static_cast<uint8>(SLOT_AMMO));
    }

    const bool ninja = PChar->GetMJob() == xi::Job::NIN || PChar->GetSJob() == xi::Job::NIN;
    if (ninja && charutils::hasSpell(PChar, static_cast<uint16>(SpellID::Utsusemi_Ichi)) && charutils::getItemCount(PChar, ITEMID::SHIHEI) < kToolStock)
    {
        script += fmt::format("p:addItem({{ id = {}, quantity = {}, silent = true }})\n", static_cast<uint16>(ITEMID::SHIHEI), stackOf(ITEMID::SHIHEI));
    }

    // Echo Drops for anyone with MP to be silenced out of, and Silent Oil and Prism Powder for everyone.
    for (const auto item : { kEchoDrops, kSilentOil, kPrismPowder })
    {
        if ((item != kEchoDrops || PChar->GetMaxMP() > 0) && charutils::getItemCount(PChar, item) < kMedicineStock)
        {
            script += fmt::format("p:addItem({{ id = {}, quantity = {}, silent = true }})\n", item, stackOf(item));
        }
    }

    if (script.empty())
    {
        return;
    }

    script = fmt::format("local p = GetPlayerByID({})\nif p == nil then return end\n{}", PChar->id, script);
    if (const auto result = lua.safe_script(script, sol::script_pass_on_error); !result.valid())
    {
        const sol::error err = result;
        ShowErrorFmt("bots: restocking {} failed: {}", bot.name, err.what());
    }
}

} // namespace bots
