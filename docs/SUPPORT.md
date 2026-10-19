# Reporting a problem

This is version 0.9: you may run into bugs, crashes or slow spots. **Any issues you encounter are no indicator of bad
porting** - DOOM 3 BFG is a large game on a very small machine, and every report helps make the next version better.

## What to send

1. **`ux0:data/doom3bfg/base/console.txt`** - the game's log. It is rewritten every time the game starts, so copy it
   **right after** the problem, before starting the game again.
2. **`ux0:data/vitaGL.log`** - the renderer's log.
3. If the Vita showed a crash ("An error has occurred"), the crash dump from `ux0:data/` (`psp2core-*.psp2dmp`), if
   there is one.
4. **Exactly what happened**: what you were doing, **where** (which game - DOOM 3, Resurrection of Evil, The Lost
   Mission, classic DOOM, multiplayer - which level, which room or area), and **when** (right at start-up, during a
   level load, after an hour of play, when a certain monster appeared, in a menu...). The more precise, the better.

## Where to send it

- the **`#support`** channel of the Ensoclock Discord server: https://discord.gg/AaXA2N4ADt
- or **GitHub Issues** on this repository

The logs are detailed on purpose: the game records what it loads, how long every frame takes and where the time
goes, memory, sound, video, network and the reason for every warning - so a problem can be found from the logs
alone.

## The L + R benchmark

Holding **L + R** at the title screen runs the built-in benchmark and verification: it plays through every part of
the game on its own (several hours), checks that everything works and writes `benchmark_results.txt`,
`verify_checks.txt` and screenshots next to `console.txt`. You don't need to run it; it is there for testing.
