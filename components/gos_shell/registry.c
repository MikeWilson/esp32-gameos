// registry.c — the game table. Adding a game touches exactly this file.
#include "gos.h"
#include "gos_core.h"

extern const gos_game_t game_gunship;
extern const gos_game_t game_golf;
extern const gos_game_t game_slots;
#ifdef GOS_HAVE_BOMBER
extern const gos_game_t game_bomber;   // in development; absent from the
#endif                                 // public tree until it ships
extern const gos_game_t game_aimtest;
extern const gos_game_t game_diag;

const gos_game_t *const g_games[] = {
    &game_gunship,
    &game_golf,
    &game_slots,
#ifdef GOS_HAVE_BOMBER
    &game_bomber,
#endif
    &game_aimtest,
    &game_diag,
};
const int g_game_count = sizeof(g_games) / sizeof(g_games[0]);
