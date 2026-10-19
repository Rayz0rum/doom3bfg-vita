# Multiplayer

DOOM 3 BFG Edition on the Vita plays every multiplayer mode of the BFG Edition - deathmatch, team deathmatch,
tourney, last man standing and capture the flag - on its maps, with up to 8 players (people and bots):

- **locally**: Vitas on the same Wi-Fi network find each other's games - nothing to set up;
- **online**: with players anywhere, no PSN account, no server to set up;
- **with bots**: alone or with friends, bots fill the empty slots.

---

> ## ⚠️ READ THIS BEFORE PLAYING ONLINE: YOUR IP ADDRESS IS VISIBLE
>
> **Unless you use a VPN or another middleman to hide your IP, your public IP address is exposed to the other
> players and to the network that is used to find them.**
>
> This online play is deliberately primitive. It was compromised specifically to give a PSN-free, easy-to-use,
> server-free online service for a console as limited as the PlayStation Vita: Vitas find each other through a
> public peer-to-peer network and then talk to each other **directly**, with no server in the middle. That is what
> makes it work without accounts, subscriptions or anyone running servers - and it is also why everyone you play
> with, and that public network, can see your internet address.
>
> **Be warned.** If that is not acceptable to you, play locally, or use a VPN.

---

## Switching between local and online play

**Settings → Game Options → Online Play: On / Off.**

- **Off** - local play: multiplayer is your local network, plus friends you added by address. Nothing goes to the
  internet.
- **On** - online play: while you are in a multiplayer lobby, the game also looks for players over the internet.

The first time you go online, the game shows the IP address warning above and asks you to pick a **username**.
Choose carefully: it is picked once, for good (other players' friends lists and bans know you by it).

## Playing locally (wireless LAN)

Local play is plain wireless LAN: every Vita connected to the **same Wi-Fi network** (the same router) sees the
others' games.

1. One player hosts: **Multiplayer → Create Match** (pick the mode, map, time and score limits, bots - see below).
2. The others: **Multiplayer → Play With Friends** (the game browser) - the host's game shows up; join it.

That's it. Online Play can stay off.

## Adding friends

Friends are kept by the game itself (not PSN), on your memory card.

- **From the game browser:** when a game shows up in **Play With Friends**, add its host as a friend from there.
  Friendship is mutual: adding someone adds you to their list too.
- **In a lobby:** open a player's profile from the lobby to add them.
- **By address:** **Play With Friends → Add Friend by Address**, and type the other player's IP address (and port,
  27015 by default). This reaches a friend's game over the internet directly; their router must let the game's
  UDP port 27015 through to their Vita (the game asks the router to do this automatically when it supports it).

Your friends' games then show up in the browser whenever they host, and you can invite them to yours.

## Playing online

1. Turn **Online Play** on (Settings → Game Options), accept the warning, pick your username.
2. Host a match, or open **Play With Friends** to see the games of players who are online.
3. Players online are found within a few seconds to a minute; your friends' games are listed first.

Routers differ: most home routers work out of the box; some (strict NAT, mobile hotspots, carrier-grade NAT) keep
two players from reaching each other. A VPN that gives your Vita its own address usually helps.

## Bots

Bots use the bot AI of **Quake III Arena** (id Software), ported to DOOM 3: they hunt, pick weapons by distance,
lead their shots, strafe, retreat to health when hurt, chase, grab items and play capture the flag. They learn each
map's paths as everyone plays it and remember them for next time.

To play with bots, **host a match** and set, in **Match Settings**:

- **Bots**: how many (None to 7). People always take the slots first: when someone joins, a bot leaves.
- **Bot Difficulty**:
  1. *I Can Win* - slow to react, poor aim, stands still in fights
  2. *Bring It On*
  3. *Hurt Me Plenty* (default)
  4. *Hardcore*
  5. *Nightmare!* - fast reactions, leads its shots, strafes and jumps

Bots work in local play and online alike. **To play alone against bots, you don't need a network at all**: turn
Online Play off (local play), host a match, add bots.

## How bans work

Online play has rules, and players who break them are banned.

- **Hosts** can ban a player from their own games (from the player's profile in the lobby).
- **The game's owner** can ban a console from **all** online play. A banned console cannot go online and is refused by
  every host.
- **Bans are tied to the console itself, not to your account or username.** Deleting your `account.dat`, making a new
  username or reinstalling the game **will not** remove a ban. Only an entirely different Vita would.

### Rules

**The same rules as in the Ensoclock Discord server - plus no cheating and no griefing.**

### Appeals

If you believe a ban was a mistake, or you want a second chance, you can appeal: join the **Ensoclock Discord server**
(https://discord.gg/AaXA2N4ADt) and submit a genuine, well-made appeal in the **`#appeal-bfg-bans`** channel. It does
not have to be pretty or professional - it has to be genuine. A template:

```
Username:
When were you banned (roughly):
What happened, in your own words:
Why the ban should be lifted:
Anything else we should know:
```

Your username and the HW ID shown in the game's ban message help find your case.
