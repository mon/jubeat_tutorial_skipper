#include <stdint.h>
#include <string.h>
#include <math.h>
#include <windows.h>
#include <GL/gl.h>

#include "third_party/minhook/include/MinHook.h"

#include "gftools.hpp"
#include "layout.hpp"
#include "log.hpp"
#include "texture.hpp"

// Scan `data[0..size]` for `pattern`. Returns nullptr if no match.
static uint8_t *find_pattern(uint8_t *data, size_t size,
                             const uint8_t *pattern, size_t pattern_size) {
    if (size < pattern_size) return nullptr;
    for (size_t i = 0; i <= size - pattern_size; i++) {
        if (memcmp(&data[i], pattern, pattern_size) == 0) return &data[i];
    }
    return nullptr;
}

// Anchor mid-function on tutorial_fsm's dispatch tail, then subtract the
// fixed distance back to the entry.
//
// We stop at the `jne` opcode and skip its 4-byte displacement — the disp
// encodes the size of the dispatch block, which shifts between builds (e.g.
// festo 0x318 vs. en_import 0x325) without changing the structure we hook.
static constexpr size_t tutorial_fsm_sig_offset = 0x41;
static const uint8_t tutorial_fsm_pattern[] = {
    0x8b, 0x43, 0x0c,                               // mov eax, [ebx+0x0C]  ; msg_id
    0x8b, 0x73, 0x10,                               // mov esi, [ebx+0x10]  ; request_ptr
    0x8b, 0x7b, 0x08,                               // mov edi, [ebx+0x08]  ; fsm
    0x89, 0xb5, 0x3c, 0xff, 0xff, 0xff,             // mov [ebp-0xC4], esi
    0x3d, 0x01, 0x00, 0x00, 0x80,                   // cmp eax, 0x80000001  ; MSG_CREATE?
    0x0f, 0x85,                                     // jne state_cases      ; disp varies
};

// GF FSM messages. The high nibble encodes the bus channel; the game dispatches
// parent bus messages (MSG_BUS_RENDER / MSG_BUS_UPDATE) into child FSMs as the
// per-tick messages below.
#define MSG_ENTER       0xB0000002  // state entered
#define MSG_LEAVE       0xB0000003  // state left
#define MSG_UPDATE_INIT 0x80000021  // first-frame update after enter
#define MSG_CREATE      0x80000001  // object construction
#define MSG_UPDATE      0xA0000023  // per-frame logic tick (no drawing)
#define MSG_RENDER      0x90000024  // per-frame render tick
#define MSG_BUS_RENDER  0x80000005  // parent-bus render, unwrapped to MSG_RENDER
#define MSG_BUS_UPDATE  0x80000006  // parent-bus update, unwrapped to MSG_UPDATE

// Tutorial FSM state ids. These live in a contiguous 0x23871000..0x2387B000
// block in jubeat.dll's state table.
#define TUT_STATE_INTRO_LOAD  0x23871000
#define TUT_STATE_BEGIN       0x23872000
#define TUT_STATE_PLAY        0x23873000
#define TUT_STATE_MUSIC_LOAD  0x23874000
#define TUT_STATE_PLAY_BEGIN  0x23875000
#define TUT_STATE_PLAY_BODY   0x23876000
#define TUT_STATE_OUT         0x23878000
#define TUT_STATE_EXIT        0x23879000
#define TUT_STATE_CLEANUP_A   0x2387A000
#define TUT_STATE_CLEANUP_B   0x2387B000
#define TUT_STATE_RANGE_END   0x23880000  // exclusive upper bound

// GL_TEXTURE_RECTANGLE_ARB isn't in the GL 1.1 headers shipped with Windows.
#ifndef GL_TEXTURE_RECTANGLE_ARB
#define GL_TEXTURE_RECTANGLE_ARB 0x84F5
#endif

static bool (*real_tutorial_fsm)(void *fsm, uint32_t state, uint32_t *request);
static void (*real_GFAVSSwapBuffers)(HDC hdc, float framerate, int swap_interval);

constexpr int FRAMES_TO_QUIT = 90;

// Watchdog: if can_hook is set but hook_tutorial_fsm stops ticking (e.g. the
// game tore the FSM down through a path we don't recognize), clear can_hook
// so the overlay doesn't get stuck
constexpr int FRAMES_FSM_STALE = 60;

// On-press button shrink. ~10% smaller while held; lerp factor of 0.7
// settles in 2-3 frames at 30fps for a snappy "physical button" press
// without snapping instantly. The ring lives in the gap exposed by the
// shrink, so its visibility is gated by the press itself.
constexpr float PRESSED_SCALE = 0.90f;
constexpr float SCALE_LERP = 0.7f;

// Progress ring: thin cyan annulus drawn behind the button, wiping in
// clockwise from 12 o'clock. Inner radius sits just outside the pressed
// button's outline (0.5 * PRESSED_SCALE = 0.45) so the ring is hidden
// under the button when not pressed and naturally revealed as the
// button shrinks.
constexpr int RING_SEGMENTS = 96;
constexpr float RING_INNER = 0.40f;
constexpr float RING_OUTER = 0.46f;  // right at the panel edge

static bool can_hook = false;
static int frames = 0;
static int frames_since_fsm = 0;
static float press_scale = 1.0f;
static bool used_minhook = false;

static constexpr float TWO_PI = 6.28318530717958647692f;
static constexpr float HALF_PI = 1.57079632679489661923f;

// Triangle-strip arc with the ring cross-section texture mapped along
// the radial direction (V=0 at inner edge, V=1 at outer). Color +
// alpha multiply the texture, so this draws the same gradient with
// whatever tint/intensity the caller specifies.
static void draw_arc(float cx, float cy, float r_in, float r_out,
                     float start, float sweep,
                     uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if(sweep <= 0.0f) return;
    int segments = (int)((sweep / TWO_PI) * RING_SEGMENTS + 0.5f);
    if(segments < 1) segments = 1;

    GFDrawBegin(GL_TRIANGLE_STRIP);
    for(int i = 0; i <= segments; i++) {
        float ang = start + sweep * (float)i / (float)segments;
        float c = cosf(ang), s = sinf(ang);
        GFDrawColor4ub(r, g, b, a); GFDrawTexCoord2f(0.f, 0.f);
        GFDrawVertex2f(cx + c * r_in,  cy + s * r_in);
        GFDrawColor4ub(r, g, b, a); GFDrawTexCoord2f(0.f, 1.f);
        GFDrawVertex2f(cx + c * r_out, cy + s * r_out);
    }
    GFDrawEnd();
}

static void draw_progress_ring(float cx, float cy, float r_in, float r_out, float t) {
    if(t <= 0.0f) return;
    GLuint tex = texture_id_ring();
    if(!tex) return;
    glBindTexture(GL_TEXTURE_2D, tex);

    float sweep = t * TWO_PI;
    if(sweep > TWO_PI) sweep = TWO_PI;
    draw_arc(cx, cy, r_in, r_out, -HALF_PI, sweep, 0xC8, 0xFF, 0xFF, 0xFF);
}

static void draw_hold_to_skip_overlay(int frames, float press_scale) {
    GLuint tex_unheld = texture_id_unheld();
    GLuint tex_held = texture_id_held();
    if(!tex_unheld || !tex_held) return;

    float px0 = LayoutGetPanelLeft(3);
    float py0 = LayoutGetPanelTop(3);
    float pw = LayoutGetPanelWidth();
    float ph = LayoutGetPanelHeight();
    float cx = px0 + pw * 0.5f;
    float cy = py0 + ph * 0.5f;

    // Press-scaled button rect. Button is at panel size when released
    // (covers the ring entirely) and shrinks via press_scale when held,
    // exposing the ring underneath.
    float bw = pw * press_scale * 0.5f;
    float bh = ph * press_scale * 0.5f;
    float bx0 = cx - bw, by0 = cy - bh;
    float bx1 = cx + bw, by1 = cy + bh;

    // Smoothstep the hold fade so it starts slow, accelerates through the
    // middle, and eases out as it approaches commit — reads as intentional
    // charging rather than a linear crossfade.
    float t = (float)frames / (float)FRAMES_TO_QUIT;
    if(t > 1.0f) t = 1.0f;
    float ease = t * t * (3.0f - 2.0f * t);
    uint8_t held_alpha = (uint8_t)(ease * 255.0f);

    // Save/restore every enable, blend, color, matrix etc. so our overlay
    // doesn't bleed into subsequent draws.
    glPushAttrib(GL_ALL_ATTRIB_BITS);

    GFDrawReadyContext();
    GFDrawReadyArrays();
    // GFDrawSetCapability's 2nd arg is an internal cached-state bitmask that
    // the engine tracks per-capability; the values come from reversing the
    // game's own overlay draws.
    GFDrawSetCapability(GL_BLEND, 2, 1);
    GFDrawSetCapability(GL_ALPHA_TEST, 1, 1);  // PNG has alpha
    GFDrawAlphaFunc(GL_GREATER, 1);            // ref 1/255
    GFDrawSetCapability(GL_TEXTURE_2D, 0x10, 1);
    GFDrawSetCapability(GL_TEXTURE_RECTANGLE_ARB, 0x20, 0); // game's default
    GFDrawBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Progress ring goes first so the button draws on top of it where they
    // overlap — keeps the button silhouette crisp.
    float r_max = (pw > ph ? pw : ph);
    draw_progress_ring(cx, cy, r_max * RING_INNER, r_max * RING_OUTER, t);

    // Base layer: unheld, always full opacity.
    glBindTexture(GL_TEXTURE_2D, tex_unheld);
    GFDrawBegin(GL_QUADS);
    GFDrawColor4ub(0xFF, 0xFF, 0xFF, 0xFF); GFDrawTexCoord2f(0.f, 0.f); GFDrawVertex2f(bx0, by0);
    GFDrawColor4ub(0xFF, 0xFF, 0xFF, 0xFF); GFDrawTexCoord2f(1.f, 0.f); GFDrawVertex2f(bx1, by0);
    GFDrawColor4ub(0xFF, 0xFF, 0xFF, 0xFF); GFDrawTexCoord2f(1.f, 1.f); GFDrawVertex2f(bx1, by1);
    GFDrawColor4ub(0xFF, 0xFF, 0xFF, 0xFF); GFDrawTexCoord2f(0.f, 1.f); GFDrawVertex2f(bx0, by1);
    GFDrawEnd();

    // Overlay: held, fades in as the skip charges.
    glBindTexture(GL_TEXTURE_2D, tex_held);
    GFDrawBegin(GL_QUADS);
    GFDrawColor4ub(0xFF, 0xFF, 0xFF, held_alpha); GFDrawTexCoord2f(0.f, 0.f); GFDrawVertex2f(bx0, by0);
    GFDrawColor4ub(0xFF, 0xFF, 0xFF, held_alpha); GFDrawTexCoord2f(1.f, 0.f); GFDrawVertex2f(bx1, by0);
    GFDrawColor4ub(0xFF, 0xFF, 0xFF, held_alpha); GFDrawTexCoord2f(1.f, 1.f); GFDrawVertex2f(bx1, by1);
    GFDrawColor4ub(0xFF, 0xFF, 0xFF, held_alpha); GFDrawTexCoord2f(0.f, 1.f); GFDrawVertex2f(bx0, by1);
    GFDrawEnd();

    glPopAttrib();
}

// The FSM manager unwraps the parent-bus messages into per-state ticks before
// re-dispatching into this function — so by the time we're called, `state` is
// the current TUT_STATE_* id and `*request` is one of MSG_ENTER / MSG_UPDATE /
// MSG_LEAVE / MSG_UPDATE_INIT.
extern "C" __cdecl bool hook_tutorial_fsm(void* fsm, uint32_t state, uint32_t *request) {
    // static uint32_t last = -1;
    // if(state != last) {
    //     log_misc("tutorial FSM %X", state);
    //     last = state;
    // }

    frames_since_fsm = 0;

    if(state == TUT_STATE_BEGIN) {
        frames = 0;
        press_scale = 1.0f;
        can_hook = true;
    }

    // TUT_STATE_EXIT is the normal completion path. IsShutting catches
    // abnormal teardown (test menu, external 0x80000003 shutdown) that would
    // otherwise skip EXIT. Don't also gate on "state outside tutorial range"
    // — this dispatcher is also called with bus-level messages
    // (state=MSG_BUS_RENDER/MSG_BUS_UPDATE = 0x80000005/6) before the
    // per-state re-dispatch, and those unsigned-compare above RANGE_END,
    // which would clear can_hook every frame.
    if(state == TUT_STATE_EXIT || state == TUT_STATE_OUT || GFFSMObjectIsShutting(fsm)) {
        can_hook = false;
    }

    bool tutorial_update = can_hook && request && *request == MSG_UPDATE
                           && state >= TUT_STATE_INTRO_LOAD
                           && state < TUT_STATE_RANGE_END;

    if(!tutorial_update)
        return real_tutorial_fsm(fsm, state, request);

    uint32_t buttons = GFInputDDevGetStateMap(2, 1);
    bool bottom_right = buttons & (1 << 15);

    // Lerp button scale toward target so a fresh press visibly "depresses"
    // over a couple of frames rather than snapping. Eyeballed lerp factor
    // settles in ~2 frames at 30fps.
    float scale_target = bottom_right ? PRESSED_SCALE : 1.0f;
    press_scale += (scale_target - press_scale) * SCALE_LERP;

    if(bottom_right) frames++;
    else frames = 0;

    if(frames >= FRAMES_TO_QUIT) {
        log_misc("Skipped");
        can_hook = false;

        GFFSMLogPutOnMessage(fsm, state, request);
        if(!GFFSMObjectIsShutting(fsm))
            GFFSMObjectSetNextState(fsm, TUT_STATE_EXIT);
        GFFSMObjectSetNextData(fsm, 0);
        GFFSMLogPutLeaveState(fsm, state, request, 1);
        GFFSMObjectSetTransiting(fsm, 1);
        return true;
    }

    return real_tutorial_fsm(fsm, state, request);
}

extern "C" __cdecl void hook_GFAVSSwapBuffers(HDC hdc, float framerate, int swap_interval) {
    // First swap = a GL context is live and AVS is booted — safe
    // point for texture upload and logging.
    static bool setup_done = false;
    if(!setup_done) {
        log_misc("backend: %s", used_minhook ? "minhook" : "patched IAT");

        setup_done = true;
        texture_init();
    }

    if(can_hook) {
        if(++frames_since_fsm >= FRAMES_FSM_STALE) can_hook = false;
        else draw_hold_to_skip_overlay(frames, press_scale);
    }

    real_GFAVSSwapBuffers(hdc, framerate, swap_interval);
}

// DllMain runs before libavs has booted, so no logging here. A non-zero
// return aborts DLL load, so a missing tutorial_fsm signature fails fast
// rather than silently shipping a build that won't skip.
int init(void) {
    HMODULE jubeat = GetModuleHandleA("jubeat.dll");
    if (!jubeat) return 3;

    // Patched mode: jubeat.dll re-exports tutorial_fsm and call sites
    // already point at our exports via its IAT.
    if (auto exported = (decltype(real_tutorial_fsm))GetProcAddress(jubeat, "tutorial_fsm")) {
        real_tutorial_fsm     = exported;
        real_GFAVSSwapBuffers = &GFAVSSwapBuffers;
        return 0;
    }

    used_minhook = true;

    if (MH_Initialize() != MH_OK) return 1;

    // note: not doing CreateHook because then it only hooks our IAT thunk, lol
    if (MH_CreateHookApi(L"gftools.dll", "GFAVSSwapBuffers",
            (LPVOID)hook_GFAVSSwapBuffers, (LPVOID*)&real_GFAVSSwapBuffers) != MH_OK) return 2;

    auto base = reinterpret_cast<uint8_t*>(jubeat);
    auto dos = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
    auto nt = reinterpret_cast<PIMAGE_NT_HEADERS>(base + dos->e_lfanew);
    size_t image_size = nt->OptionalHeader.SizeOfImage;

    uint8_t *hit = find_pattern(base, image_size,
                                tutorial_fsm_pattern, sizeof(tutorial_fsm_pattern));
    if (!hit) return 4;

    void *target = hit - tutorial_fsm_sig_offset;
    if (MH_CreateHook(target, (LPVOID)hook_tutorial_fsm,
                      (LPVOID*)&real_tutorial_fsm) != MH_OK) return 5;

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) return 2;
    return 0;
}
