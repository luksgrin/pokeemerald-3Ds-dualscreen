/*
 * The game's link-counter constants (include/constants/local_link.h, made by
 * the wireless patch), for host builds of 3ds_local.c before the game tree
 * is bootstrapped. The compile pre-defines the real header's guard
 * (GUARD_CONSTANTS_LOCAL_LINK_H) and includes this one instead.
 */
#ifndef GUARD_CONSTANTS_LOCAL_LINK_H
#define GUARD_CONSTANTS_LOCAL_LINK_H

#define LOCAL_SESSION_OFF       0 // local wireless isn't set up
#define LOCAL_SESSION_SEARCHING 1 // looking for a session nearby
#define LOCAL_SESSION_IN        2 // joined one, or hosting one
#define LOCAL_SESSION_FAILED    3 // the wireless couldn't be started (switched off?)
#define LOCAL_SESSION_NONE      4 // not in a session
#define LOCAL_SESSION_OTHER_BUILD 5 // the players nearby have another build of the game

// Which counter a session is for: players only meet others at the same kind
// of counter, as the GBA's wireless features only see their own kind.
#define LOCAL_KIND_ANY            0 // sessions started for tests
#define LOCAL_KIND_UNION_ROOM     1
#define LOCAL_KIND_DIRECT_CORNER  2
#define LOCAL_KIND_GAME_CORNER    3 // Mossdeep Game Corner: Pokemon Jump, Dodrio Berry-Picking
#define LOCAL_KIND_BERRY_BLENDER  4 // Lilycove Contest Hall's Berry Blenders
#define LOCAL_KIND_CONTEST        5 // Lilycove Contest Hall's link contest receptionist
#define LOCAL_KIND_BATTLE_TOWER   6 // Battle Tower link multi battles

#endif
