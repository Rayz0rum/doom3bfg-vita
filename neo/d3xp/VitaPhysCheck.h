/*
===========================================================================

Gameplay checks for the game logic's tick rate (vita_gameHz).

DOOM 3 was tuned at 60 game tics a second; the Vita runs it at 30. These
measure what the player and the world actually do - so a log can show they
behave as at 60 Hz, and a PC build can measure both rates side by side
(-DID_VITA_PHYSCHECK, console command vitaPhysReport):

  player   full-stick ground speed running / walking / crouched (against
           pm_runspeed / pm_walkspeed / pm_crouchspeed), jump heights
           (against pm_jumpheight)
  weapons  sustained fire rate of each weapon
  ragdolls each one's time to come to rest, and whether vita_afMaxMoveTime
           had to stop it; "body stuck" corrections
  objects  each moveable's time to rest, distance and top speed; any that
           leave the world's bounds
  doors    each binary mover's travel time against its set duration

Definitions in Game_local.cpp.

===========================================================================
*/
#ifndef __VITA_PHYSCHECK_H__
#define __VITA_PHYSCHECK_H__

#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
#define VITA_PHYSCHECK 1

void Vita_PhysCheck_Player(bool onGround, bool crouched, bool running, int forwardmove, int rightmove, bool jumpHeld, const idVec3& velocity, float z);
void Vita_PhysCheck_Shot(const char* weaponName);
void Vita_PhysCheck_RagdollActive();
void Vita_PhysCheck_RagdollRest(const char* name, float seconds, bool forced, float distance, float topSpeed);
void Vita_PhysCheck_RagdollStuck();
void Vita_PhysCheck_RagdollLong(const char* name, float seconds, float linSpeed, float angSpeed, float linAcc, float angAcc, int contacts, const char* touching);
void Vita_PhysCheck_ObjectRest(const char* name, float seconds, float distance, float topSpeed);
void Vita_PhysCheck_ObjectOutOfWorld(const char* name, const idVec3& origin);
void Vita_PhysCheck_Door(const char* name, int elapsedMs, int expectedMs);
void Vita_PhysCheck_Report(); // prints the summary lines and starts a new window
#endif

#endif /* !__VITA_PHYSCHECK_H__ */
