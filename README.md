# lsb-bots

Bot players for a [LandSandBoat](https://github.com/LandSandBoat/server) FFXI server. Bots log in as ordinary
characters, take invites, follow, fight, heal and pull, form bot-only parties and hunt, and chat through any
OpenAI-style chat completions endpoint (a local llama-server works).

Each bot is a `CCharEntity` with its own session that never goes on the wire. It acts by sending client packets
through the server's own packet handlers, so the rest of the server treats it as a player.

## Install

1. Clone this repo into the server's `modules/` directory as `bots`:

   ```sh
   git clone git@github.com:lenonk/lsb-bots.git modules/bots
   ```

2. Add a line with `bots` to `modules/init.txt`.
3. Copy `settings/bots.lua.example` to the server's `settings/bots.lua`. It can't keep the `.lua` name inside the
   module, because the server runs every `.lua` file under a module directory as a module script.
4. Rebuild `xi_map`:

   ```sh
   cmake --build build --config RelWithDebInfo --target xi_map
   ```

## The pool

The bots are the rows of the `bot_pool` table, which the module creates on its first start. A row with
`charid = 0` gets an account (`bot<accid>`, which can't be logged into) and a character at the next start.

```sql
INSERT INTO bot_pool (name, job, level, race, face, nation, zone, personality, chattiness)
VALUES ('Aldwin', 1, 20, 1, 2, 0, 230, 'Cheerful Hume warrior who loves a good fight.', 50);
```

## Chat

`CHAT_ENABLED`, `LLM_URL`, `LLM_API_KEY` and `LLM_MODEL` in `settings/bots.lua` choose the endpoint. The defaults
point at a llama-server on `127.0.0.1:8085`. Bots answer tells, party and linkshell chat, and answer say, shout and
yell when someone names them. With `LLM_ORDERS`, the model reads party chat for requests such as "Name pull",
"camp here" or "can someone cure me", and C++ checks every plan before a bot acts on it. Without it, keywords decide.

`bot_pool.personality` holds each bot's personality. `bot_pool.chattiness` (0-100) sets how often it speaks unasked.

## Commands

- `!bots`: who is online and what they're doing.
- `!bot <name>`: one bot's state and the reason for it.
- `!rotatebots [count]`: bots outside a party log out and others take their place. With no count, all of them rotate.
