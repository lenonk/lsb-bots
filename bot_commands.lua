-----------------------------------
-- !bots, !bot <name> and !rotatebots (modules/bots)
-- BotsStatus, BotsInspect and BotsRotate are bound by the C++ module.
-----------------------------------
require('modules/module_utils')
-----------------------------------

local function reply(player, text)
    player:printToPlayer(text, xi.msg.channel.SYSTEM_3, '')
end

---@type TCommand
local status = {}

status.cmdprops =
{
    permission = 0,
    parameters = '',
}

status.onTrigger = function(player)
    for _, line in ipairs(BotsStatus()) do
        reply(player, line)
    end
end

xi.module.registerCommand('bots', status)

---@type TCommand
local inspect = {}

inspect.cmdprops =
{
    permission = 0,
    parameters = 's',
}

-- !bot <name>: what that bot is doing and why.
inspect.onTrigger = function(player, name)
    if name == nil then
        reply(player, '!bot <name>')
        return
    end
    for _, line in ipairs(BotsInspect(name)) do
        reply(player, line)
    end
end

xi.module.registerCommand('bot', inspect)

---@type TCommand
local rotate = {}

rotate.cmdprops =
{
    permission = 0,
    parameters = 'i',
}

-- !rotatebots [count]: bots outside a party log out and others take their place. No count rotates all of them.
rotate.onTrigger = function(player, count)
    for _, line in ipairs(BotsRotate(count)) do
        reply(player, line)
    end
end

xi.module.registerCommand('rotatebots', rotate)
