//
// Bot pool: the bot_pool table, and the accounts and characters behind it.
//

#include "bots.h"

#include "common/database.h"
#include "common/logging.h"

#include <array>
#include <string_view>

namespace bots
{

namespace
{

// char_jobs columns, indexed by job id
constexpr std::array<std::string_view, 23> jobColumns = {
    "",
    "war", "mnk", "whm", "blm", "rdm", "thf", "pld", "drk", "bst", "brd", "rng",
    "sam", "nin", "drg", "smn", "blu", "cor", "pup", "dnc", "sch", "geo", "run",
};

auto nextId(const char* query) -> uint32
{
    const auto rset = db::preparedStmt(query);
    FOR_DB_SINGLE_RESULT(rset)
    {
        return rset->get<uint32>("id") + 1;
    }

    return 0;
}

auto nameTaken(const std::string& name) -> bool
{
    const auto rset = db::preparedStmt("SELECT charid FROM chars WHERE charname = ? LIMIT 1", name);
    return !rset || rset->rowsCount() > 0;
}

// Mirrors loginHelpers::saveCharacter, plus the level, position and home point the pool row asks for.
auto createCharacter(const std::string& name) -> bool
{
    const auto rset = db::preparedStmt("SELECT race, face, size, nation, job, level, zone, pos_x, pos_y, pos_z, pos_rot "
                                       "FROM bot_pool WHERE name = ?",
                                       name);
    if (!rset || !rset->next())
    {
        return false;
    }

    const auto race   = rset->get<uint8>("race");
    const auto face   = rset->get<uint8>("face");
    const auto size   = rset->get<uint8>("size");
    const auto nation = rset->get<uint8>("nation");
    const auto job    = rset->get<uint8>("job");
    const auto level  = rset->get<uint8>("level");
    const auto zone   = rset->get<uint16>("zone");
    const auto x      = rset->get<float>("pos_x");
    const auto y      = rset->get<float>("pos_y");
    const auto z      = rset->get<float>("pos_z");
    const auto rot    = rset->get<uint8>("pos_rot");

    if (job == 0 || job >= jobColumns.size())
    {
        ShowErrorFmt("bots: {} has invalid job {}", name, job);
        return false;
    }

    if (nameTaken(name))
    {
        ShowErrorFmt("bots: the name {} is already taken by another character", name);
        return false;
    }

    const auto accId  = nextId("SELECT MAX(id) AS id FROM accounts");
    const auto charId = nextId("SELECT MAX(charid) AS id FROM chars");
    if (accId == 0 || charId == 0)
    {
        return false;
    }

    // status 0 and an impossible hash: the login server refuses this account.
    if (!db::preparedStmt("INSERT INTO accounts (id, login, password, timecreate, status) VALUES (?, ?, '!', NOW(), 0)",
                          accId,
                          fmt::format("bot{}", accId)))
    {
        return false;
    }

    const bool ok =
        db::preparedStmt("INSERT INTO chars (charid, accid, charname, pos_zone, nation, pos_x, pos_y, pos_z, pos_rot, "
                         "home_zone, home_x, home_y, home_z, home_rot) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                         charId, accId, name, zone, nation, x, y, z, rot, zone, x, y, z, rot) &&
        db::preparedStmt("INSERT INTO char_look (charid, face, race, size) VALUES (?, ?, ?, ?)", charId, face, race, size) &&
        db::preparedStmt("INSERT INTO char_stats (charid, mjob) VALUES (?, ?)", charId, job) &&
        // The chars insert trigger already made most of these rows.
        db::preparedStmt("INSERT INTO char_exp (charid) VALUES (?) ON DUPLICATE KEY UPDATE charid = charid", charId) &&
        db::preparedStmt("INSERT INTO char_flags (charid) VALUES (?) ON DUPLICATE KEY UPDATE disconnecting = disconnecting", charId) &&
        db::preparedStmt("INSERT INTO char_jobs (charid) VALUES (?) ON DUPLICATE KEY UPDATE charid = charid", charId) &&
        db::preparedStmt("INSERT INTO char_points (charid) VALUES (?) ON DUPLICATE KEY UPDATE charid = charid", charId) &&
        db::preparedStmt("INSERT INTO char_unlocks (charid) VALUES (?) ON DUPLICATE KEY UPDATE charid = charid", charId) &&
        db::preparedStmt("INSERT INTO char_profile (charid) VALUES (?) ON DUPLICATE KEY UPDATE charid = charid", charId) &&
        db::preparedStmt("INSERT INTO char_storage (charid) VALUES (?) ON DUPLICATE KEY UPDATE charid = charid", charId) &&
        db::preparedStmt("DELETE FROM char_inventory WHERE charid = ?", charId) &&
        db::preparedStmt("INSERT INTO char_inventory (charid) VALUES (?)", charId) &&
        db::preparedStmt(fmt::format("UPDATE char_jobs SET unlocked = unlocked | ?, {0} = ?, genkai = GREATEST(genkai, {0}) WHERE charid = ?", jobColumns[job]),
                         1u << job, level, charId) &&
        db::preparedStmt("UPDATE bot_pool SET charid = ?, accid = ? WHERE name = ?", charId, accId, name);

    if (!ok)
    {
        ShowErrorFmt("bots: creating {} failed part way (account {}, char {})", name, accId, charId);
        return false;
    }

    ShowInfoFmt("bots: created {} (account {}, char {}, job {} level {})", name, accId, charId, jobColumns[job], level);
    return true;
}

} // namespace

void ensureSchema()
{
    db::preparedStmt("CREATE TABLE IF NOT EXISTS bot_pool ("
                     "name varchar(15) NOT NULL, "
                     "enabled tinyint(1) unsigned NOT NULL DEFAULT 1, "
                     "charid int(10) unsigned NOT NULL DEFAULT 0, "
                     "accid int(10) unsigned NOT NULL DEFAULT 0, "
                     "race tinyint(2) unsigned NOT NULL DEFAULT 1, "
                     "face tinyint(2) unsigned NOT NULL DEFAULT 0, "
                     "size tinyint(1) unsigned NOT NULL DEFAULT 0, "
                     "nation tinyint(1) unsigned NOT NULL DEFAULT 0, "
                     "job tinyint(2) unsigned NOT NULL DEFAULT 1, "
                     "level tinyint(2) unsigned NOT NULL DEFAULT 1, "
                     "zone smallint(3) unsigned NOT NULL DEFAULT 230, "
                     "pos_x float NOT NULL DEFAULT 0, "
                     "pos_y float NOT NULL DEFAULT 0, "
                     "pos_z float NOT NULL DEFAULT 0, "
                     "pos_rot tinyint(3) unsigned NOT NULL DEFAULT 0, "
                     "personality text NULL, "
                     "chattiness tinyint(3) unsigned NULL, "
                     "PRIMARY KEY (name)"
                     ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");

    // Added after the table first shipped.
    db::preparedStmt("ALTER TABLE bot_pool ADD COLUMN IF NOT EXISTS chattiness tinyint(3) unsigned NULL AFTER personality");
    db::preparedStmt("ALTER TABLE bot_pool ADD COLUMN IF NOT EXISTS subjob tinyint(2) unsigned NULL AFTER level");
}

auto loadPool() -> std::vector<PoolEntry>
{
    std::vector<std::string> missing;

    if (const auto rset = db::preparedStmt("SELECT name FROM bot_pool WHERE enabled = 1 AND charid = 0"))
    {
        while (rset->next())
        {
            missing.emplace_back(rset->get<std::string>("name"));
        }
    }

    for (const auto& name : missing)
    {
        createCharacter(name);
    }

    std::vector<PoolEntry> pool;

    const auto rset = db::preparedStmt("SELECT b.name, b.charid, b.accid, s.mjob, s.mlvl FROM bot_pool b "
                                       "JOIN chars c ON c.charid = b.charid "
                                       "JOIN char_stats s ON s.charid = b.charid "
                                       "WHERE b.enabled = 1");
    FOR_DB_MULTIPLE_RESULTS(rset)
    {
        pool.push_back(PoolEntry{
            .name   = rset->get<std::string>("name"),
            .charId = rset->get<uint32>("charid"),
            .accId  = rset->get<uint32>("accid"),
            .job    = rset->get<uint8>("mjob"),
            .level  = rset->get<uint8>("mlvl"),
        });
    }

    return pool;
}

// A crashed map server leaves bot sessions and party rows behind.
void clearStaleState()
{
    db::preparedStmt("DELETE s FROM accounts_sessions s JOIN bot_pool b ON b.charid = s.charid");
    db::preparedStmt("DELETE p FROM accounts_parties p JOIN bot_pool b ON b.charid = p.charid");
}

auto countRealPlayers() -> uint32
{
    const auto rset = db::preparedStmt("SELECT COUNT(*) AS n FROM accounts_sessions s "
                                       "LEFT JOIN bot_pool b ON b.charid = s.charid WHERE b.charid IS NULL");
    FOR_DB_SINGLE_RESULT(rset)
    {
        return rset->get<uint32>("n");
    }

    return 0;
}

namespace
{
std::set<uint32> poolChars;
} // namespace

void registerPoolChar(const uint32 charId)
{
    poolChars.insert(charId);
}

auto isPoolChar(const uint32 charId) -> bool
{
    return poolChars.contains(charId);
}

} // namespace bots
