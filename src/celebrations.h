#pragma once
// Done celebrations: a short particle animation over the whole screen after
// "Erledigt", picked at random from a set so it stays fresh, never the same one
// twice in a row.
//
// Built on what plans/A-particle-celebration.md worked out for this panel:
// - The particles are not LVGL objects. One transparent full-screen object on
//   the top layer draws them all in its DRAW_MAIN callback, from a static array.
//   One object per particle would cost LV_MEM per piece and two invalid areas
//   per move.
// - Every animation step invalidates one area per particle (the union of its
//   old and new box), so a frame redraws only a few thousand pixels over SPI.
//   CEL_MAX stays below LVGL's 32 invalid-area slots (LV_INV_BUF_SIZE); an
//   overflow would only cost one full-screen frame.
// - No transforms, style opa or layers: only lv_draw_rect / lv_draw_label with
//   a descriptor opacity, which blends straight into the draw buffer.
// - The overlay is never clickable, so taps fall through to the screen below:
//   "Rückgängig" stays usable during the whole celebration.
// - The overlay stays visible all the time and draws nothing while idle, so
//   starting and ending a celebration costs no full-screen frame.
//
// Include after type_color() and the WasteType enum.

#include <lvgl.h>

#define CEL_MAX 24

enum CelShape : uint8_t
{
    CS_RECT,    // confetti piece
    CS_DOT,     // filled circle
    CS_SPARKLE, // thin plus sign
    CS_CHECK    // LV_SYMBOL_OK glyph
};

struct CelParticle
{
    int16_t x0, y0;   // start, px
    int16_t vx, vy;   // px/s
    int16_t phase;    // degrees, per-particle variation
    uint16_t delay;   // ms before it appears
    uint8_t size;     // px
    uint8_t shape;    // CelShape
    lv_color_t color;
    lv_area_t box;    // last drawn box, valid while vis
    lv_opa_t opa;     // opacity belonging to box
    bool vis;
};

struct CelPose
{
    int32_t x, y; // centre
    int32_t w, h;
    lv_opa_t opa;
};

typedef bool (*cel_pose_fn)(const CelParticle &p, int32_t t, CelPose &o); // t: ms since its delay
typedef void (*cel_init_fn)(const lv_area_t &spawn, const lv_color_t *bins, int nb);

struct CelKind
{
    const char *name;
    uint16_t duration_ms;
    cel_init_fn init;
    cel_pose_fn pose;
};

static CelParticle cel_p[CEL_MAX];
static int cel_n = 0;
static lv_obj_t *cel_overlay = NULL;
static const CelKind *cel_kind = NULL;
static int cel_last = -1;
static int celebration_force = -1; // >= 0: always this kind (test hooks, host renders)

static const uint32_t cel_palette_hex[] = {0xFFD54F, 0x81C784, 0x4FC3F7, 0xF06292, 0xFFFFFF, 0xFF8A65, 0xBA68C8};
#define CEL_PALETTE_N (sizeof(cel_palette_hex) / sizeof(cel_palette_hex[0]))

static lv_color_t cel_rand_color()
{
    return lv_color_hex(cel_palette_hex[lv_rand(0, CEL_PALETTE_N - 1)]);
}

static int32_t cel_rand(int32_t lo, int32_t hi)
{
    return lo + (int32_t)lv_rand(0, (uint32_t)(hi - lo));
}

static int32_t cel_sin(int32_t deg, int32_t amp) // amp * sin(deg)
{
    deg %= 360;
    if (deg < 0)
    {
        deg += 360;
    }
    return (lv_trigo_sin((int16_t)deg) * amp) >> LV_TRIGO_SHIFT;
}

static lv_opa_t cel_fade(int32_t t, int32_t start, int32_t end) // 255 until start, 0 at end
{
    if (t <= start)
    {
        return LV_OPA_COVER;
    }
    if (t >= end)
    {
        return LV_OPA_TRANSP;
    }
    return (lv_opa_t)(255 - (t - start) * 255 / (end - start));
}

static CelParticle &cel_add()
{
    static CelParticle spare; // an init that asks for too many writes here and draws nothing
    if (cel_n >= CEL_MAX)
    {
        return spare;
    }
    CelParticle &p = cel_p[cel_n++];
    memset(&p, 0, sizeof(p));
    return p;
}


// ---------------------------------------------------------------- 1 confetti

// pieces burst out of the bin bars, in the bins' colors plus a few bright ones,
// and fall under gravity; flipping is faked by changing the width
static void confetti_init(const lv_area_t &spawn, const lv_color_t *bins, int nb)
{
    for (int i = 0; i < 24; i++)
    {
        CelParticle &p = cel_add();
        p.x0 = cel_rand(spawn.x1 + 8, spawn.x2 - 8);
        p.y0 = cel_rand(spawn.y1 + 4, spawn.y2 - 4);
        p.vx = (p.x0 - 160) / 2 + cel_rand(-60, 60);
        p.vy = -cel_rand(160, 320);
        p.phase = cel_rand(0, 359);
        p.delay = cel_rand(0, 80);
        p.shape = CS_RECT;
        p.color = (i % 3 == 2 || nb == 0) ? cel_rand_color() : bins[i % nb];
    }
}

static bool confetti_pose(const CelParticle &p, int32_t t, CelPose &o)
{
    o.x = p.x0 + p.vx * t / 1000;
    o.y = p.y0 + p.vy * t / 1000 + 300 * t / 1000 * t / 1000; // g = 600 px/s²
    o.w = 2 + LV_ABS(cel_sin(p.phase + t * 3 / 4, 9));
    o.h = 8;
    o.opa = cel_fade(t, 1150, 1400);
    return o.y < 250;
}


// ---------------------------------------------------------------- 2 balloons

static void balloons_init(const lv_area_t &spawn, const lv_color_t *bins, int nb)
{
    for (int i = 0; i < 10; i++)
    {
        CelParticle &p = cel_add();
        p.x0 = 20 + i * 28 + cel_rand(-6, 6);
        p.size = cel_rand(14, 24);
        p.y0 = 240 + p.size;
        p.vy = -cel_rand(170, 260);
        p.phase = cel_rand(0, 359);
        p.delay = cel_rand(0, 450);
        p.shape = CS_DOT;
        p.color = cel_rand_color();
    }
}

static bool balloons_pose(const CelParticle &p, int32_t t, CelPose &o)
{
    o.x = p.x0 + cel_sin(p.phase + t * 2 / 5, 10); // gentle sway
    o.y = p.y0 + p.vy * t / 1000;
    o.w = p.size;
    o.h = p.size + p.size / 6; // a little taller than wide
    o.opa = cel_fade(t, 1100, 1300);
    return o.y > -p.size;
}


// ---------------------------------------------------------------- 3 fireworks

// two bursts of dots flying out from a point, slowing down, sagging, fading
static void fireworks_init(const lv_area_t &spawn, const lv_color_t *bins, int nb)
{
    for (int b = 0; b < 2; b++)
    {
        int16_t cx = (b == 0) ? cel_rand(60, 140) : cel_rand(180, 260);
        int16_t cy = cel_rand(45, 110);
        lv_color_t c = cel_rand_color();
        int16_t a0 = cel_rand(0, 32);
        for (int i = 0; i < 11; i++)
        {
            CelParticle &p = cel_add();
            p.x0 = cx;
            p.y0 = cy;
            int16_t ang = a0 + i * 360 / 11;
            int16_t speed = cel_rand(200, 250);
            p.vx = cel_sin(ang + 90, speed);
            p.vy = cel_sin(ang, speed);
            p.delay = b * 380;
            p.size = 8;
            p.shape = CS_DOT;
            p.color = c;
        }
    }
}

static bool fireworks_pose(const CelParticle &p, int32_t t, CelPose &o)
{
    const int32_t T = 900; // the dots stop flying out after T ms
    int32_t tc = LV_MIN(t, T);
    int32_t k = tc - tc * tc / (2 * T); // distance factor in ms: decelerates to 0 at T
    o.x = p.x0 + p.vx * k / 1000;
    o.y = p.y0 + p.vy * k / 1000 + 60 * t / 1000 * t / 1000; // slow sag
    o.w = o.h = (t < 120) ? 3 + t * (p.size - 3) / 120 : p.size;
    o.opa = cel_fade(t, 500, 950);
    return t < 950;
}


// ---------------------------------------------------------------- 4 sparkles

// sparkles twinkle up and away all over the screen, one after another
static void sparkles_init(const lv_area_t &spawn, const lv_color_t *bins, int nb)
{
    for (int i = 0; i < 16; i++)
    {
        CelParticle &p = cel_add();
        p.x0 = cel_rand(16, 304);
        p.y0 = cel_rand(14, 226);
        p.size = cel_rand(14, 22);
        p.delay = cel_rand(0, 750);
        p.shape = CS_SPARKLE;
        static const uint32_t warm[] = {0xFFFFFF, 0xFFE082, 0xFFF59D, 0xA5D6A7};
        p.color = lv_color_hex(warm[lv_rand(0, 3)]);
    }
}

static bool sparkles_pose(const CelParticle &p, int32_t t, CelPose &o)
{
    const int32_t life = 560;
    if (t >= life)
    {
        return false;
    }
    o.x = p.x0;
    o.y = p.y0;
    o.w = o.h = 2 + cel_sin(t * 180 / life, p.size); // grows and shrinks
    o.opa = LV_OPA_COVER;
    return o.w > 2;
}


// ---------------------------------------------------------------- 5 check rain

static void checkrain_init(const lv_area_t &spawn, const lv_color_t *bins, int nb)
{
    for (int i = 0; i < 14; i++)
    {
        CelParticle &p = cel_add();
        p.x0 = 12 + i * 22 + cel_rand(-5, 5);
        p.y0 = -12;
        p.vy = cel_rand(230, 330);
        p.phase = cel_rand(0, 359);
        p.delay = cel_rand(0, 500);
        p.shape = CS_CHECK;
        p.color = (i % 2) ? lv_color_hex(0x81C784) : lv_color_white();
    }
}

static bool checkrain_pose(const CelParticle &p, int32_t t, CelPose &o)
{
    o.x = p.x0 + cel_sin(p.phase + t / 2, 6);
    o.y = p.y0 + p.vy * t / 1000;
    o.w = 20;
    o.h = 20;
    o.opa = cel_fade(t, 900, 1100);
    return o.y < 252;
}


// ---------------------------------------------------------------- 6 shooting stars

// four comets streak diagonally across the screen, each a head and a fading tail
static void comets_init(const lv_area_t &spawn, const lv_color_t *bins, int nb)
{
    for (int c = 0; c < 4; c++)
    {
        int16_t x0 = cel_rand(-40, 140);
        int16_t y0 = cel_rand(-10, 90);
        int16_t vx = cel_rand(330, 400);
        int16_t vy = vx * 2 / 5 + cel_rand(-20, 20);
        lv_color_t col = (c % 2) ? lv_color_hex(0xFFE082) : lv_color_white();
        for (int j = 0; j < 5; j++)
        {
            CelParticle &p = cel_add();
            p.x0 = x0;
            p.y0 = y0;
            p.vx = vx;
            p.vy = vy;
            p.delay = c * 170 + j * 30; // tail dots follow the head on the same path
            static const uint8_t sz[5] = {11, 9, 7, 6, 4};
            p.size = sz[j];
            p.phase = j; // tail index, for the opacity
            p.shape = CS_DOT;
            p.color = col;
        }
    }
}

static bool comets_pose(const CelParticle &p, int32_t t, CelPose &o)
{
    o.x = p.x0 + p.vx * t / 1000;
    o.y = p.y0 + p.vy * t / 1000;
    o.w = o.h = p.size;
    o.opa = (lv_opa_t)(255 - p.phase * 45);
    return o.x < 330 && o.y < 250;
}


// ---------------------------------------------------------------- engine

static const CelKind cel_kinds[] = {
    {"confetti", 1450, confetti_init, confetti_pose},
    {"balloons", 1800, balloons_init, balloons_pose},
    {"fireworks", 1350, fireworks_init, fireworks_pose},
    {"sparkles", 1350, sparkles_init, sparkles_pose},
    {"check rain", 1600, checkrain_init, checkrain_pose},
    {"shooting stars", 1450, comets_init, comets_pose},
};
#define CEL_KINDS ((int)(sizeof(cel_kinds) / sizeof(cel_kinds[0])))

static void cel_box(const CelPose &o, lv_area_t &a)
{
    a.x1 = o.x - o.w / 2 - 1;
    a.y1 = o.y - o.h / 2 - 1;
    a.x2 = a.x1 + o.w + 2;
    a.y2 = a.y1 + o.h + 2;
}

static void cel_draw(lv_event_t *e)
{
    if (cel_kind == NULL)
    {
        return;
    }
    lv_layer_t *layer = lv_event_get_layer(e);
    for (int i = 0; i < cel_n; i++)
    {
        const CelParticle &p = cel_p[i];
        const lv_area_t &c = layer->_clip_area; // public member in 9.2.2 (lv_draw.h)
        if (!p.vis || p.box.x2 < c.x1 || p.box.x1 > c.x2 || p.box.y2 < c.y1 || p.box.y1 > c.y2)
        {
            continue; // most particles lie outside the area being redrawn
        }
        lv_area_t a = {p.box.x1 + 1, p.box.y1 + 1, p.box.x2 - 1, p.box.y2 - 1}; // cel_box adds 1 px
        if (p.shape == CS_CHECK)
        {
            lv_draw_label_dsc_t d;
            lv_draw_label_dsc_init(&d);
            d.text = LV_SYMBOL_OK;
            d.font = &lv_font_montserrat_16;
            d.color = p.color;
            d.opa = p.opa;
            d.align = LV_TEXT_ALIGN_CENTER;
            lv_draw_label(layer, &d, &a);
            continue;
        }
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = p.color;
        d.bg_opa = p.opa;
        if (p.shape == CS_SPARKLE)
        {
            int32_t cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2;
            d.radius = 1;
            lv_area_t hz = {a.x1, cy - 1, a.x2, cy};
            lv_area_t vt = {cx - 1, a.y1, cx, a.y2};
            lv_draw_rect(layer, &d, &hz);
            lv_draw_rect(layer, &d, &vt);
            continue;
        }
        d.radius = (p.shape == CS_DOT) ? LV_RADIUS_CIRCLE : 1;
        lv_draw_rect(layer, &d, &a);
    }
}

static void cel_invalidate_all()
{
    for (int i = 0; i < cel_n; i++)
    {
        if (cel_p[i].vis)
        {
            lv_obj_invalidate_area(cel_overlay, &cel_p[i].box);
            cel_p[i].vis = false;
        }
    }
}

// one animation step: new pose per particle, one invalid area per moved particle
static void cel_step(void *var, int32_t t)
{
    if (cel_kind == NULL)
    {
        return;
    }
    for (int i = 0; i < cel_n; i++)
    {
        CelParticle &p = cel_p[i];
        CelPose o;
        bool vis = t >= p.delay && cel_kind->pose(p, t - p.delay, o) && o.opa > LV_OPA_MIN;
        lv_area_t nb;
        if (vis)
        {
            cel_box(o, nb);
            if (p.vis && nb.x1 == p.box.x1 && nb.y1 == p.box.y1 && nb.x2 == p.box.x2 && nb.y2 == p.box.y2 &&
                o.opa == p.opa)
            {
                continue; // unchanged: nothing to redraw
            }
        }
        if (p.vis && vis)
        {
            lv_area_t u;
            u.x1 = LV_MIN(p.box.x1, nb.x1);
            u.y1 = LV_MIN(p.box.y1, nb.y1);
            u.x2 = LV_MAX(p.box.x2, nb.x2);
            u.y2 = LV_MAX(p.box.y2, nb.y2);
            lv_obj_invalidate_area(cel_overlay, &u);
        }
        else if (p.vis)
        {
            lv_obj_invalidate_area(cel_overlay, &p.box);
        }
        else if (vis)
        {
            lv_obj_invalidate_area(cel_overlay, &nb);
        }
        p.vis = vis;
        if (vis)
        {
            p.box = nb;
            p.opa = o.opa;
        }
    }
}

static void cel_done(lv_anim_t *a)
{
    cel_invalidate_all();
    Serial.printf("celebration %s done\n", cel_kind ? cel_kind->name : "-");
    cel_kind = NULL;
    cel_n = 0;
}

static bool celebration_running()
{
    return cel_kind != NULL;
}

// ends a running celebration at once (undo, or a new one starting)
static void celebration_stop()
{
    if (cel_kind == NULL)
    {
        return;
    }
    lv_anim_delete(cel_overlay, cel_step);
    cel_invalidate_all();
    cel_kind = NULL;
    cel_n = 0;
}

// spawn: area of the hero bars (confetti starts there); bins: colors of the day's bins
static void celebration_start(const lv_area_t &spawn, const lv_color_t *bins, int nb)
{
    if (cel_overlay == NULL)
    {
        return;
    }
    celebration_stop();
    static bool seeded = false;
    if (!seeded)
    {
        lv_rand_set_seed(millis() * 2654435761u + 1); // the time of the first tap varies
        seeded = true;
    }
    int k;
    if (celebration_force >= 0 && celebration_force < CEL_KINDS)
    {
        k = celebration_force;
    }
    else if (cel_last < 0)
    {
        k = (int)lv_rand(0, CEL_KINDS - 1);
    }
    else
    {
        k = (cel_last + (int)lv_rand(1, CEL_KINDS - 1)) % CEL_KINDS; // never the same twice in a row
    }
    cel_last = k;
    cel_kind = &cel_kinds[k];
    cel_n = 0;
    cel_kind->init(spawn, bins, nb);
    Serial.printf("celebration %s, %d particles\n", cel_kind->name, cel_n);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, cel_overlay);
    lv_anim_set_exec_cb(&a, cel_step);
    lv_anim_set_values(&a, 0, cel_kind->duration_ms);
    lv_anim_set_duration(&a, cel_kind->duration_ms);
    lv_anim_set_path_cb(&a, lv_anim_path_linear); // the value is the time in ms
    lv_anim_set_completed_cb(&a, cel_done);
    lv_anim_start(&a);
}

// once at boot, after the screen: the overlay stays visible and empty while idle
static void celebration_setup()
{
    cel_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(cel_overlay); // transparent, no border, no padding
    lv_obj_set_size(cel_overlay, 320, 240);
    lv_obj_set_pos(cel_overlay, 0, 0);
    lv_obj_clear_flag(cel_overlay, LV_OBJ_FLAG_CLICKABLE); // taps reach "Rückgängig"
    lv_obj_clear_flag(cel_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(cel_overlay, cel_draw, LV_EVENT_DRAW_MAIN, NULL);
}
