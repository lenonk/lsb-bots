//
// The bots' linkshell (settings LINKSHELL): every other pool character wears its pearl, and a player
// who asks one of them for a pearl by tell gets one of their own.
//

#include "bots.h"

#include "common/database.h"
#include "common/logging.h"
#include "common/settings.h"

#include "map/entities/char_entity.h"
#include "map/item_container.h"
#include "map/items/item_linkshell.h"
#include "map/linkshell.h"
#include "map/utils/charutils.h"
#include "map/utils/zoneutils.h"

#include <algorithm>
#include <cctype>

namespace bots
{

namespace
{

constexpr uint16 kColor = 0xF5A3; // the pearl's colour

uint32 shellId = 0;

// Every other pool character, so the same ones carry it from one start to the next.
auto member(const uint32 charId) -> bool
{
    return charId % 2 == 0;
}

auto wearing(const CCharEntity* PChar) -> bool
{
    return shellId != 0 && ((PChar->PLinkshell1 != nullptr && PChar->PLinkshell1->getID() == shellId) ||
                            (PChar->PLinkshell2 != nullptr && PChar->PLinkshell2->getID() == shellId));
}

// A pearl of the bots' linkshell in the inventory.
auto findPearl(CCharEntity* PChar) -> CItemLinkshell*
{
    auto* PInventory = PChar->getStorage(LOC_INVENTORY);
    for (uint8 slot = 0; slot <= PInventory->GetSize(); ++slot)
    {
        if (auto* PPearl = dynamic_cast<CItemLinkshell*>(PInventory->GetItem(slot)); PPearl != nullptr && PPearl->GetLSID() == shellId)
        {
            return PPearl;
        }
    }
    return nullptr;
}

} // namespace

void ensureLinkshell()
{
    // Letters only, as the game allows; the name also goes into a Lua script as it is.
    const auto name = settings::get<std::string>("bots.LINKSHELL");
    if (name.empty() || !std::ranges::all_of(name, [](const char c) { return std::isalpha(static_cast<unsigned char>(c)); }))
    {
        if (!name.empty())
        {
            ShowWarningFmt("bots: linkshell name {} isn't letters only; no linkshell", name);
        }
        return;
    }

    if (const auto rset = db::preparedStmt("SELECT linkshellid FROM linkshells WHERE name = ? AND broken = 0", name); rset && rset->next())
    {
        shellId = rset->get<uint32>("linkshellid");
    }
    else
    {
        shellId = linkshell::RegisterNewLinkshell(name, kColor);
    }
    ShowInfoFmt("bots: linkshell {} is {}", name, shellId != 0 ? fmt::format("id {}", shellId) : std::string("unavailable"));
}

auto offersPearls(const Bot& bot) -> bool
{
    const auto* PChar = bot.entity();
    return PChar != nullptr && wearing(PChar);
}

// Real players in the linkshell right now, by name.
auto linkshellPlayers() -> std::vector<std::string>
{
    std::vector<std::string> names;
    if (auto* PShell = shellId != 0 ? linkshell::GetLinkshell(shellId) : nullptr)
    {
        for (const auto* PMember : PShell->members)
        {
            if (!isPoolChar(PMember->id))
            {
                names.push_back(PMember->getName());
            }
        }
    }
    return names;
}

void linkshellUpkeep(Bot& bot, sol::state& lua, const timer::time_point tick)
{
    auto* PChar = bot.entity();
    if (shellId == 0 || PChar == nullptr || !throttle(bot, 0x90000, 0, std::chrono::seconds(30), tick))
    {
        return;
    }

    const auto  name   = settings::get<std::string>("bots.LINKSHELL");
    std::string script = fmt::format("local ls = '{}'\n", name);
    const bool  wanted = member(bot.charId) && PChar->PLinkshell1 == nullptr;
    if (wanted && findPearl(PChar) == nullptr)
    {
        script += fmt::format("do local p = GetPlayerByID({}); if p ~= nil then p:addLinkpearl(ls, false) end end\n", PChar->id);
    }

    // Players who asked for one; names are letters only, so they go into the script as they are.
    for (const auto& player : bot.pearlFor)
    {
        auto* PPlayer = zoneutils::GetCharByName(player);
        if (PPlayer == nullptr || isPoolChar(PPlayer->id) || wearing(PPlayer) || findPearl(PPlayer) != nullptr ||
            !std::ranges::all_of(player, [](const char c) { return std::isalpha(static_cast<unsigned char>(c)); }))
        {
            continue;
        }
        script += fmt::format("do local p = GetPlayerByName('{}'); if p ~= nil then p:addLinkpearl(ls, false) end end\n", player);
        ShowInfoFmt("bots: {} gives {} a linkpearl", bot.name, player);
    }
    bot.pearlFor.clear();

    if (const auto result = lua.safe_script(script, sol::script_pass_on_error); !result.valid())
    {
        const sol::error err = result;
        ShowErrorFmt("bots: linkpearls for {} failed: {}", bot.name, err.what());
    }

    // Worn in the first slot, the way addLinkpearl equips the second.
    if (wanted)
    {
        if (auto* PPearl = findPearl(PChar); PPearl != nullptr && linkshell::AddOnlineMember(PChar, PPearl, 1))
        {
            if (PChar->bindEquip(SLOT_LINK1, PPearl))
            {
                charutils::SaveCharEquip(PChar);
            }
            else
            {
                linkshell::DelOnlineMember(PChar, PPearl);
            }
        }
    }
}

} // namespace bots
