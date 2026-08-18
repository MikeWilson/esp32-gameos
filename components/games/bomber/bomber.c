// bomber.c — BOMBER's contract, state machine, and simulation.
//
// The physical BOOT button (GOS_BTN_A) is the whole interface philosophy:
// press to commit to a straight-and-level bomb run, press to release. Tilt
// steers in cruise and trims the sight. Flak accuracy ramps against
// continuous time-in-sight (wall clock, suspend-aware) — leaving the sight
// resets it, so every run is a stay-or-bail bet. Fixed 1/60 dt simulation;
// all user-facing durations on gos_time_us().

#include "bomber_int.h"

bomber_t *G;
uint16_t *CV;

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------

#define SFX(name, prio_, ...) \
    static const gos_note_t name##_n[] = { __VA_ARGS__ }; \
    static const gos_sfx_t name = { name##_n, sizeof(name##_n) / sizeof(gos_note_t), prio_ }

SFX(sfx_engine, 1,  { 62, 62, 460, GOS_W_TRI, 88 }, { 58, 58, 460, GOS_W_TRI, 88 });
SFX(sfx_clunk, 3,   { 300, 180, 40, GOS_W_NOISE, 170 }, { 140, 120, 70, GOS_W_TRI, 190 });
SFX(sfx_whistle, 2, { 1900, 380, 1350, GOS_W_SQ25, 95 });
SFX(sfx_crump, 3,   { 2200, 300, 40, GOS_W_NOISE, 220 }, { 62, 26, 480, GOS_W_TRI, 255 },
                    { 200, 90, 240, GOS_W_NOISE, 120 });
SFX(sfx_flak_pop, 2, { 1500, 500, 60, GOS_W_NOISE, 160 });
SFX(sfx_flak_crack, 2, { 3200, 900, 45, GOS_W_NOISE, 200 });
SFX(sfx_hit, 4,     { 2600, 400, 90, GOS_W_NOISE, 235 }, { 180, 120, 160, GOS_W_TRI, 220 });
SFX(sfx_klaxon, 4,  { 700, 700, 120, GOS_W_SQ50, 150 }, { 520, 520, 120, GOS_W_SQ50, 150 },
                    { 700, 700, 120, GOS_W_SQ50, 150 });
SFX(sfx_destroy, 4, { 523, 523, 70, GOS_W_SQ25, 150 }, { 659, 659, 70, GOS_W_SQ25, 155 },
                    { 880, 880, 150, GOS_W_SQ25, 165 });
SFX(sfx_bail, 2,    { 900, 500, 90, GOS_W_SQ25, 120 });
SFX(sfx_debrief, 4, { 392, 392, 90, GOS_W_SQ25, 145 }, { 523, 523, 90, GOS_W_SQ25, 150 },
                    { 659, 659, 200, GOS_W_SQ25, 160 });
SFX(sfx_title, 3,   { 262, 262, 80, GOS_W_SQ25, 130 }, { 392, 392, 80, GOS_W_SQ25, 135 },
                    { 523, 523, 200, GOS_W_SQ25, 145 });
SFX(sfx_tap, 1,     { 1050, 1050, 24, GOS_W_SQ50, 120 });
SFX(sfx_tick, 1,    { 1500, 1500, 14, GOS_W_SQ25, 100 });

void bomber_sfx(int id)
{
    static const gos_sfx_t *tab[] = {
        [BSFX_CLUNK] = &sfx_clunk,       [BSFX_WHISTLE] = &sfx_whistle,
        [BSFX_CRUMP] = &sfx_crump,       [BSFX_FLAK_POP] = &sfx_flak_pop,
        [BSFX_FLAK_CRACK] = &sfx_flak_crack, [BSFX_HIT] = &sfx_hit,
        [BSFX_DESTROY] = &sfx_destroy,   [BSFX_BAIL] = &sfx_bail,
        [BSFX_DEBRIEF] = &sfx_debrief,   [BSFX_TITLE] = &sfx_title,
        [BSFX_TAP] = &sfx_tap,           [BSFX_TICK] = &sfx_tick,
        [BSFX_KLAXON] = &sfx_klaxon,
    };
    gos_audio_play(tab[id], 200);
}

// ---------------------------------------------------------------------------
// Persistence: top-5 board
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t version, count;
    char last_ini[3];
    uint8_t pad;
    board_entry_t board[BOARD_SIZE];
} bomber_save_t;

static void save_board(gos_ctx_t *ctx)
{
    bomber_save_t s = { .version = 1, .count = (uint8_t)G->board_count };
    memcpy(s.last_ini, G->entry_ini, 3);
    memcpy(s.board, G->board, sizeof s.board);
    gos_save_write(ctx, &s, sizeof s);
}

static void load_board(gos_ctx_t *ctx)
{
    bomber_save_t s;
    int n = gos_save_read(ctx, &s, sizeof s);
    if (n == (int)sizeof s && s.version == 1) {
        G->board_count = s.count > BOARD_SIZE ? BOARD_SIZE : s.count;
        memcpy(G->entry_ini, s.last_ini, 3);
        memcpy(G->board, s.board, sizeof G->board);
    } else {
        G->entry_ini[0] = 'A'; G->entry_ini[1] = 'A'; G->entry_ini[2] = 'A';
    }
    for (int i = 0; i < 3; i++)
        if (G->entry_ini[i] < 'A' || G->entry_ini[i] > 'Z') G->entry_ini[i] = 'A';
}

static void board_insert(gos_ctx_t *ctx)
{
    board_entry_t e;
    memcpy(e.ini, G->entry_ini, 3);
    e.ini[3] = 0;
    e.score = (uint32_t)G->score;
    e.sorties = (uint16_t)G->sortie_num;
    e.targets = (uint16_t)G->run_targets;
    int pos = G->board_count;
    for (int i = 0; i < G->board_count; i++)
        if (e.score > G->board[i].score) { pos = i; break; }
    if (pos < BOARD_SIZE) {
        int end = G->board_count < BOARD_SIZE ? G->board_count : BOARD_SIZE - 1;
        for (int i = end; i > pos; i--) G->board[i] = G->board[i - 1];
        G->board[pos] = e;
        if (G->board_count < BOARD_SIZE) G->board_count++;
    }
    save_board(ctx);
}

// ---------------------------------------------------------------------------
// Sortie / run flow
// ---------------------------------------------------------------------------

static void add_burst(float x, float y, int kind)
{
    for (int i = 0; i < MAX_BURSTS; i++) {
        if (G->bursts[i].alive) continue;
        G->bursts[i] = (burst_t){ .x = x, .y = y, .age = 0, .kind = (uint8_t)kind,
                                  .alive = true };
        return;
    }
}

static void start_sortie(int num)
{
    G->sortie_num = num;
    G->sortie_seed = gos_rand(G->hwrng);
    G->plane_x = STRIP_W / 2;
    G->plane_y = 0;
    G->plane_vx = 0;
    G->bank = 0;
    G->cam_x = G->plane_x - SCREEN_W / 2;
    G->bombs_left = MAX_BOMBS;
    G->sortie_hits = G->sortie_drops = G->sortie_kills = 0;
    G->out_of_bombs_us = 0;
    G->shake = 0;
    memset(G->bombs, 0, sizeof G->bombs);
    memset(G->bursts, 0, sizeof G->bursts);
    terrain_reset(G->sortie_seed);
    terrain_gen_to(SCREEN_H + GEN_AHEAD);   // first screens, in one burst
}

static void enter_state(bomber_state_t st)
{
    G->st = st;
    G->card_drawn = false;
    G->card_us = gos_time_us();
}

static void start_run(void)
{
    G->score = 0;
    G->damage = 0;
    G->run_targets = 0;
    start_sortie(1);
    enter_state(BST_BRIEF);
}

static void end_run(const char *reason)
{
    G->end_reason = reason;
    if (G->h_engine >= 0) { gos_audio_stop(G->h_engine); G->h_engine = -1; }
    bomber_sfx(BSFX_DEBRIEF);
    enter_state(BST_RUN_END);
}

// ---------------------------------------------------------------------------
// Combat resolution
// ---------------------------------------------------------------------------

static void destroy_target(target_t *t)
{
    t->state = TS_DEAD;
    if (t->kind != TGT_CONVOY) terrain_stamp_dead(t);
    G->score += (int)(100 * (1.0f + 0.25f * (G->sortie_num - 1)));
    G->run_targets++;
    G->sortie_kills++;
    bomber_sfx(BSFX_DESTROY);
}

static void bomb_impact(bomb_t *b)
{
    terrain_stamp_crater(b->x, b->y);
    add_burst(b->x, b->y, 1);
    bomber_sfx(BSFX_CRUMP);
    G->shake = 5;

    for (int i = 0; i < G->n_targets; i++) {
        target_t *t = &G->targets[i];
        if (t->state == TS_DEAD) continue;
        if (t->kind == TGT_CONVOY) {
            // per-truck checks; convoy dies when its trucks do
            int left = 0;
            for (int k = 0; k < G->n_trucks; k++) {
                truck_t *tr = &G->trucks[k];
                if (!tr->alive || tr->target_ix != i) continue;
                float tx = road_cx(tr->y);
                float d = sqrtf((tx - b->x) * (tx - b->x) + (tr->y - b->y) * (tr->y - b->y));
                if (d < HIT_NEAR * 0.7f) {
                    tr->alive = false;
                    terrain_stamp_crater(tx, tr->y);
                    G->score += 40;
                    G->sortie_hits++;
                } else {
                    left++;
                }
            }
            if (left == 0) destroy_target(t);
            continue;
        }
        float d = sqrtf((t->x - b->x) * (t->x - b->x) + (t->y - b->y) * (t->y - b->y));
        if (d <= HIT_DIRECT) {
            G->sortie_hits++;
            G->score += 50;   // direct-hit bonus
            destroy_target(t);
        } else if (d <= HIT_NEAR) {
            G->sortie_hits++;
            if (t->state == TS_INTACT) t->state = TS_BURNING;
            else destroy_target(t);   // burning + another hit = gone
        }
    }
}

static void take_damage(float amount)
{
    float before = G->damage;
    G->damage = bclampf(G->damage + amount, 0, 100);
    G->shake = fmaxf(G->shake, 3 + amount * 0.3f);
    bomber_sfx(BSFX_HIT);
    if (before < 40 && G->damage >= 40) bomber_sfx(BSFX_KLAXON);
    if (before < 70 && G->damage >= 70) bomber_sfx(BSFX_KLAXON);
    if (G->damage >= 100) end_run("SHOT DOWN");
}

static void flak_tick(int64_t now)
{
    if (G->st != BST_SIGHT) return;   // cruise takes no flak damage (v1)
    float sigma = FLAK_SIGMA_MIN +
                  FLAK_SIGMA_RANGE * expf(-G->t_sight / FLAK_RAMP_TAU);
    for (int i = 0; i < G->n_flak; i++) {
        flak_t *f = &G->flak[i];
        if (fabsf(f->y - G->plane_y) > 400) continue;
        if (f->next_fire_us == 0)
            f->next_fire_us = now + FLAK_PERIOD_US / 2;
        if (now < f->next_fire_us) continue;
        f->next_fire_us = now + FLAK_PERIOD_US;

        float bx, by;
        uint32_t h = gos_rand(G->hwrng);
        float g1 = ((h & 0xFFFF) / 65536.0f - 0.5f) +
                   (((h >> 16) & 0xFFFF) / 65536.0f - 0.5f);
        uint32_t h2 = gos_rand(G->hwrng);
        float g2 = ((h2 & 0xFFFF) / 65536.0f - 0.5f) +
                   (((h2 >> 16) & 0xFFFF) / 65536.0f - 0.5f);
        if (!f->telegraphed) {
            // guaranteed wide first shot with a distinct crack: the warning
            f->telegraphed = true;
            bx = G->plane_x + (g1 > 0 ? 1 : -1) * (90 + fabsf(g1) * 60);
            by = G->plane_y + g2 * 120;
            bomber_sfx(BSFX_FLAK_CRACK);
        } else {
            bx = G->plane_x + g1 * sigma * 1.6f;
            by = G->plane_y + g2 * sigma * 1.6f;
            bomber_sfx(BSFX_FLAK_POP);
        }
        add_burst(bx, by, 0);
        float d = sqrtf((bx - G->plane_x) * (bx - G->plane_x) +
                        (by - G->plane_y) * (by - G->plane_y));
        if (d <= 30) take_damage(10 + (h % 7u));
        else if (d <= 70) take_damage(3 + (h % 4u));
    }
}

// ---------------------------------------------------------------------------
// Per-frame simulation shared by CRUISE and SIGHT
// ---------------------------------------------------------------------------

static void sim_common(float dt, int64_t now)
{
    // wind: base + gusts (amplitude and wander scale with the sortie)
    G->gust_t += dt;
    float ga = 0.4f + bclampf(0.05f * G->sortie_num, 0.0f, 0.3f);
    float mag = 1.0f + ga * sinf(G->gust_t * 0.9f) * sinf(G->gust_t * 0.23f + 1.7f);
    float wander = 0.5f * sinf(G->gust_t * 0.31f);
    float cw = cosf(wander), sw = sinf(wander);
    G->wind_x = (G->wind_base_x * cw - G->wind_base_y * sw) * mag;
    G->wind_y = (G->wind_base_x * sw + G->wind_base_y * cw) * mag;

    // forward flight (the sight trims airspeed slightly)
    float vy = CRUISE_SPEED * (G->st == BST_SIGHT ? 1.0f + G->trim_speed : 1.0f);
    G->plane_y += vy * dt;
    G->plane_x = bclampf(G->plane_x + G->plane_vx * dt, 24, STRIP_W - 24);

    // camera follows laterally
    float want = bclampf(G->plane_x - SCREEN_W / 2, 0, STRIP_W - SCREEN_W);
    G->cam_x += (want - G->cam_x) * 0.08f;

    // generate terrain ahead
    terrain_gen_to((int)cam_top() + GEN_AHEAD);

    // convoys crawl toward the player
    for (int i = 0; i < G->n_trucks; i++)
        if (G->trucks[i].alive) G->trucks[i].y -= 25.0f * dt;
    for (int i = 0; i < G->n_targets; i++)
        if (G->targets[i].kind == TGT_CONVOY && G->targets[i].state != TS_DEAD) {
            // track the convoy center for brackets/aim
            float cy = 0; int n = 0;
            for (int k = 0; k < G->n_trucks; k++)
                if (G->trucks[k].alive && G->trucks[k].target_ix == i) {
                    cy += G->trucks[k].y; n++;
                }
            if (n) G->targets[i].y = cy / n;
        }

    // bombs in flight (live wind keeps acting after release — gusts are
    // the miss source, leading them is the skill)
    for (int i = 0; i < BOMBS_INFLIGHT; i++) {
        bomb_t *b = &G->bombs[i];
        if (!b->alive) continue;
        b->x += (b->vx + G->wind_x) * dt;
        b->y += (b->vy + G->wind_y) * dt;
        b->t_left -= dt;
        if (b->t_left <= 0) {
            b->alive = false;
            bomb_impact(b);
        }
    }

    // bursts age out
    for (int i = 0; i < MAX_BURSTS; i++) {
        if (!G->bursts[i].alive) continue;
        G->bursts[i].age += dt;
        if (G->bursts[i].age > 1.1f) G->bursts[i].alive = false;
    }

    G->shake = fmaxf(0.0f, G->shake - 14.0f * dt);
    flak_tick(now);

    // engine sputter when badly damaged
    if (G->h_engine >= 0) {
        int vol = 88;
        if (G->damage >= 70)
            vol = 60 + (int)(28 * (0.5f + 0.5f * sinf(now * 9e-6f)));
        gos_audio_set_vol(G->h_engine, (uint8_t)vol);
    }

    // sortie end conditions
    if (G->plane_y > SORTIE_ROWS) {
        if (G->st == BST_SIGHT) { /* fall through to debrief anyway */ }
        if (G->h_engine >= 0) { gos_audio_stop(G->h_engine); G->h_engine = -1; }
        bomber_sfx(BSFX_DEBRIEF);
        enter_state(BST_DEBRIEF);
        return;
    }
    bool inflight = false;
    for (int i = 0; i < BOMBS_INFLIGHT; i++) inflight |= G->bombs[i].alive;
    bool remaining = false;
    for (int i = 0; i < G->n_targets; i++)
        remaining |= G->targets[i].state != TS_DEAD;
    if (G->bombs_left == 0 && !inflight && remaining) {
        if (G->out_of_bombs_us == 0) G->out_of_bombs_us = now;
        else if (now - G->out_of_bombs_us > 4000000) end_run("OUT OF BOMBS");
    } else {
        G->out_of_bombs_us = 0;
    }
}

// ---------------------------------------------------------------------------
// Contract
// ---------------------------------------------------------------------------

static void bomber_init(gos_ctx_t *ctx)
{
    G = ctx->state;
    G->hwrng = &ctx->rng;
    CV = gos_gfx_fb565();
    memset(CV, 0, SCREEN_W * SCREEN_H * 2);
    load_board(ctx);
    G->h_engine = -1;
    G->sortie_num = 1;
    G->sortie_seed = gos_rand(&ctx->rng);
    G->plane_x = STRIP_W / 2;
    G->cam_x = G->plane_x - SCREEN_W / 2;
    terrain_reset(G->sortie_seed);
    terrain_gen_to(SCREEN_H + GEN_AHEAD);
    G->title_reseed_us = gos_time_us();
    bomber_sfx(BSFX_TITLE);
    enter_state(BST_TITLE);
}

static void bomber_update(gos_ctx_t *ctx, float dt, const gos_input_t *in)
{
    int64_t now = gos_time_us();
    G->aim_mode = (uint8_t)gos_input_get_aim_mode();

    // full-res touch with tap detection (parallax-tolerant: taps are edges
    // with small movement; drags belong to TOUCH_DRAG aiming)
    int tx = in->touch.x * 2, ty = in->touch.y * 2;
    bool press = in->touching && !G->touch_was;
    bool release = !in->touching && G->touch_was;
    G->touch_was = in->touching;
    if (press) { G->press_x = (int16_t)tx; G->press_y = (int16_t)ty; G->press_us = now; }
    bool tap = release &&
               abs(tx - G->press_x) < 24 && abs(ty - G->press_y) < 24 &&
               (now - G->press_us) < 500000;
    bool btn = (in->pressed & GOS_BTN_A) != 0;

    switch (G->st) {
    case BST_TITLE: {
        // attract: autopilot drift over the countryside, reseed every ~20 s
        G->title_drift += dt;
        G->plane_vx = sinf(G->title_drift * 0.3f) * 40.0f;
        G->plane_y += CRUISE_SPEED * 0.7f * dt;
        G->plane_x = bclampf(G->plane_x + G->plane_vx * dt, 100, STRIP_W - 100);
        float want = bclampf(G->plane_x - SCREEN_W / 2, 0, STRIP_W - SCREEN_W);
        G->cam_x += (want - G->cam_x) * 0.05f;
        terrain_gen_to((int)cam_top() + GEN_AHEAD);
        if (now - G->title_reseed_us > 20000000 || G->plane_y > SORTIE_ROWS - 600) {
            G->sortie_seed = gos_rand(G->hwrng);
            G->plane_y = 0;
            terrain_reset(G->sortie_seed);
            terrain_gen_to(SCREEN_H + GEN_AHEAD);
            G->title_reseed_us = now;
        }
        if (btn || tap) {
            bomber_sfx(BSFX_TAP);
            start_run();
        }
        break;
    }
    case BST_BRIEF:
        if (!G->card_drawn) { draw_brief_card(); G->card_drawn = true; }
        // let the player read it; A or tap begins once the strip has depth
        if ((btn || tap) && now - G->card_us > 400000 &&
            G->gen_row > SCREEN_H + GEN_AHEAD - 1) {
            bomber_sfx(BSFX_CLUNK);
            if (G->h_engine < 0) G->h_engine = gos_audio_loop(&sfx_engine, 88);
            G->st = BST_CRUISE;
        }
        break;
    case BST_CRUISE: {
        // tilt roll steers; the sprite banks toward the command
        float steer = in->aim_x;
        G->plane_vx = steer * MAX_LATERAL;
        G->bank += (steer * 0.35f - G->bank) * 0.12f;
        sim_common(dt, now);
        if (G->st != BST_CRUISE) break;   // sortie ended inside sim
        if ((btn || tap) && G->bombs_left > 0) {
            bomber_sfx(BSFX_CLUNK);
            G->trim_x = 0;
            G->trim_speed = 0;
            G->sight_enter_us = now;
            G->last_us = now;
            G->t_sight = 0;
            G->sight_backdrop_drawn = false;
            G->plane_vx = 0;
            G->a_down_us = 0;      // the entry press's hold never bails
            G->a_bailed = false;
            G->st = BST_SIGHT;
        }
        break;
    }
    case BST_SIGHT: {
        // straight and level: no steering. Tilt is bombsight trim.
        G->t_sight += (float)(now - G->last_us) * 1e-6f;   // wall clock,
        G->last_us = now;                                   // suspend-aware
        G->trim_x = in->aim_x * TRIM_LATERAL;
        G->trim_speed = -in->aim_y * TRIM_SPEED;   // tilt away = drop long
        G->bank += (0.0f - G->bank) * 0.2f;
        sim_common(dt, now);
        if (G->st != BST_SIGHT) break;
        {
            bool drop = false, bail = false;
            // button: quick click = drop (on release, still crisp);
            // holding ~1/3 s = bail without spending a bomb
            if (btn) { G->a_down_us = now; G->a_bailed = false; }
            if ((in->buttons & GOS_BTN_A) && G->a_down_us && !G->a_bailed &&
                now - G->a_down_us >= 300000) {
                G->a_bailed = true;
                bail = true;
            }
            if ((in->released & GOS_BTN_A) && G->a_down_us && !G->a_bailed)
                drop = true;
            // touch: the FIRE button drops; any other tap = bail out
            // (generous zone, biased up for the panel's low-thumb parallax)
            if (tap) {
                if (tx >= FIRE_BTN_X - 62 && tx <= FIRE_BTN_X + 62 &&
                    ty >= FIRE_BTN_Y - FIRE_BTN_R - 14) drop = true;
                else bail = true;
            }
            if (drop) {
                if (G->bombs_left > 0) {
                    for (int i = 0; i < BOMBS_INFLIGHT; i++) {
                        if (G->bombs[i].alive) continue;
                        G->bombs[i] = (bomb_t){
                            .x = G->plane_x, .y = G->plane_y,
                            .vx = G->trim_x,
                            .vy = CRUISE_SPEED * (1.0f + G->trim_speed),
                            .t_left = BOMB_TOF, .alive = true,
                        };
                        G->bombs_left--;
                        G->sortie_drops++;
                        bomber_sfx(BSFX_WHISTLE);
                        break;
                    }
                } else {
                    bomber_sfx(BSFX_TICK);
                }
            }
            if (bail) {   // bombs in flight keep falling
                bomber_sfx(BSFX_BAIL);
                G->t_sight = 0;
                G->st = BST_CRUISE;
            }
        }
        break;
    }
    case BST_DEBRIEF:
        if (!G->card_drawn) { draw_debrief_card(); G->card_drawn = true; }
        if ((btn || tap) && now - G->card_us > 600000) {
            bomber_sfx(BSFX_TAP);
            start_sortie(G->sortie_num + 1);
            enter_state(BST_BRIEF);
        }
        break;
    case BST_RUN_END:
        if (!G->card_drawn) { draw_run_end_card(); G->card_drawn = true; }
        if ((btn || tap) && now - G->card_us > 800000) {
            bomber_sfx(BSFX_TAP);
            enter_state(BST_INITIALS);
        }
        break;
    case BST_INITIALS:
        if (!G->card_drawn) { draw_initials_card(); G->card_drawn = true; }
        if (btn || tap) {
            // button = file the record; taps work the letter columns too
            if (btn || ty >= 324) {
                bomber_sfx(BSFX_TAP);
                board_insert(ctx);
                G->sortie_seed = gos_rand(G->hwrng);
                G->sortie_num = 1;
                G->plane_y = 0;
                G->plane_x = STRIP_W / 2;
                terrain_reset(G->sortie_seed);
                terrain_gen_to(SCREEN_H + GEN_AHEAD);
                G->title_reseed_us = now;
                bomber_sfx(BSFX_TITLE);
                enter_state(BST_TITLE);
            } else {
                for (int i = 0; i < 3; i++) {
                    int cx2 = 114 + i * 70;
                    if (tx < cx2 - 35 || tx > cx2 + 35) continue;
                    if (ty < 150 || ty > 316) break;
                    int step = ty < 233 ? 1 : 25;
                    G->entry_ini[i] = (char)('A' + (G->entry_ini[i] - 'A' + step) % 26);
                    bomber_sfx(BSFX_TICK);
                    draw_initials_card();
                    break;
                }
            }
        }
        break;
    }
}

static void bomber_render(gos_ctx_t *ctx)
{
    (void)ctx;
    switch (G->st) {
    case BST_TITLE:
        render_title();
        break;
    case BST_CRUISE:
        render_cruise();
        break;
    case BST_SIGHT:
        if (!G->sight_backdrop_drawn) {
            sight_backdrop();
            G->sight_backdrop_drawn = true;
        }
        render_sight();
        break;
    default:
        break;   // cards: drawn once, canvas holds
    }
}

static void bomber_suspend(gos_ctx_t *ctx)
{
    if (G->h_engine >= 0) { gos_audio_stop(G->h_engine); G->h_engine = -1; }
    save_board(ctx);
}

static void bomber_resume(gos_ctx_t *ctx)
{
    (void)ctx;
    G->last_us = gos_time_us();   // pause must not accrue sight time
    if (G->st == BST_CRUISE || G->st == BST_SIGHT)
        G->h_engine = gos_audio_loop(&sfx_engine, 88);
    if (G->st == BST_SIGHT) G->sight_backdrop_drawn = false;
    G->card_drawn = false;        // redraw held cards over the overlay's frame
}

static void bomber_teardown(gos_ctx_t *ctx)
{
    save_board(ctx);
    gos_audio_stop_all();
}

#ifdef GOS_HOST_SIM
void bomber_sim_probe(int *st, int *sortie, float *px, float *py, int *bombs,
                      float *dmg, int *score, int *ntgt_alive, float *wx, float *wy)
{
    *st = (int)G->st;
    *sortie = G->sortie_num;
    *px = G->plane_x; *py = G->plane_y;
    *bombs = G->bombs_left;
    *dmg = G->damage;
    *score = G->score;
    int n = 0;
    for (int i = 0; i < G->n_targets; i++)
        if (G->targets[i].state != TS_DEAD) n++;
    *ntgt_alive = n;
    *wx = G->wind_x; *wy = G->wind_y;
}
void bomber_sim_target(int ix, float *x, float *y, int *state, int *defended)
{
    if (ix >= G->n_targets) { *state = -1; return; }
    *x = G->targets[ix].x; *y = G->targets[ix].y;
    *state = G->targets[ix].state;
    *defended = G->targets[ix].defended;
}
#endif

const gos_game_t game_bomber = {
    .id = "bomber",
    .title = "BOMBER",
    .caps = GOS_CAP_IMU | GOS_CAP_AUDIO | GOS_CAP_SAVE | GOS_CAP_FB565,
    .state_size = sizeof(bomber_t),
    .init = bomber_init,
    .update = bomber_update,
    .render = bomber_render,
    .suspend = bomber_suspend,
    .resume = bomber_resume,
    .teardown = bomber_teardown,
};
