#include <stdio.h>
#include <sys/time.h>
// #include <alsa/asoundlib.h>  // Miyoo Mini: audio disabled for now
#include <SDL/SDL.h>
#include "crash_handler.h"

#include "game.h"

SDL_Surface *screen = NULL;

#define SCREEN_WIDTH    640  // Miyoo Mini Plus panel
#define SCREEN_HEIGHT   480  // Miyoo Mini Plus panel
#ifdef __MIYOO__
#define BTN_A           SDLK_LALT
#define BTN_B           SDLK_LCTRL
#define BTN_X           SDLK_LSHIFT
#define BTN_Y           SDLK_SPACE
#define BTN_L1          SDLK_BACKSPACE
#define BTN_R1          SDLK_TAB
#define BTN_L2          SDLK_RSHIFT
#define BTN_R2          SDLK_RALT
#else
#define BTN_B           SDLK_SPACE
#define BTN_A           SDLK_LCTRL
#define BTN_TA          SDLK_LALT
#define BTN_TB          SDLK_LSHIFT
#endif
#define BTN_START       SDLK_RETURN
#define BTN_SELECT      SDLK_ESCAPE
#define BTN_R           SDLK_RCTRL
#define BTN_UP          SDLK_UP
#define BTN_DOWN        SDLK_DOWN
#define BTN_LEFT        SDLK_LEFT
#define BTN_RIGHT       SDLK_RIGHT

// timing
unsigned int startTime;

int osGetTimeMS() {
    timeval t;
    gettimeofday(&t, NULL);
    return int((t.tv_sec - startTime) * 1000 + t.tv_usec / 1000);
}

// sound
// Miyoo Mini: play through the SoC's own audio output (MI_AO), the same way
// steward-fu's SDL2 port does (src/audio/mini/SDL_audio_mini.c). The ALSA
// device accepts parameter sets it does not honour, which played the game
// pitched down and distorted; MI_AO takes the mixer's 44.1 kHz stereo as is.
#include <unistd.h>
#include <mi_sys.h>
#include <mi_ao.h>

#define SND_RATE    44100
#define SND_SAMPLES 1024

static MI_AUDIO_DEV    sndDev = 0;
static MI_AO_CHN       sndChn = 0;
static MI_AUDIO_Attr_t sndAttr;
static pthread_t       sndThread;
static volatile bool   sndRunning = false;
Sound::Frame          *sndData = NULL;

// The audio block appears to run at 48 kHz: fed 44.1 kHz, MI_AO loads its
// own sample rate converter (_MI_AO_OpenSrcLib), which seems to restart on
// every block, leaving a jump at each boundary - a click once per block
// whose size follows the loudness. Convert here instead, with one
// continuous resampler across blocks, and hand MI_AO its native rate.
#define SND_OUT_RATE  44100  // 48 kHz played too fast: not the native rate
#define SND_SRC_CHUNK 512

static Sound::Frame sndSrc[SND_SRC_CHUNK];
static int          sndSrcPos = SND_SRC_CHUNK;
static Sound::Frame sndPrev, sndCur;
static double       sndPhase  = 1.0;

static inline Sound::Frame sndNextFrame() {
    if (sndSrcPos >= SND_SRC_CHUNK) {
        Sound::fill(sndSrc, SND_SRC_CHUNK);
        sndSrcPos = 0;
    }
    return sndSrc[sndSrcPos++];
}

static void sndResample(Sound::Frame *out, int count) {
    const double step = double(SND_RATE) / double(SND_OUT_RATE);
    for (int i = 0; i < count; i++) {
        while (sndPhase >= 1.0) {
            sndPrev = sndCur;
            sndCur  = sndNextFrame();
            sndPhase -= 1.0;
        }
        out[i].L = int16(sndPrev.L + (sndCur.L - sndPrev.L) * sndPhase);
        out[i].R = int16(sndPrev.R + (sndCur.R - sndPrev.R) * sndPhase);
        sndPhase += step;
    }
}

#define SND_RING 8
static Sound::Frame sndRing[SND_RING][SND_SAMPLES];

static void* sndLoop(void *arg) {
    int slot = 0;
    int logs = 0;
    while (sndRunning) {
        // Rotate buffers so a block is never overwritten while the output
        // might still be reading it.
        Sound::Frame *buf = sndRing[slot];
        slot = (slot + 1) % SND_RING;
        // Fill straight from the mixer, one whole block per call. Asking the
        // mixer for smaller pieces (512 frames, via the resampler) made it
        // drop a little decoded audio at some call boundaries: a click at
        // each one and music running fast. At 44.1 kHz no resampling is
        // needed anyway.
        Sound::fill(buf, SND_SAMPLES);

        MI_AUDIO_Frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.eBitwidth    = sndAttr.eBitwidth;
        frame.eSoundmode   = sndAttr.eSoundmode;
        frame.u32Len       = SND_SAMPLES * sizeof(Sound::Frame);
        frame.apVirAddr[0] = buf;
        frame.apVirAddr[1] = NULL;

        // Wait for real room in the output queue, then send exactly once.
        // Resending after a failed call risked queueing a block twice; like a
        // reused buffer, that makes a jump at a block boundary - a click once
        // per block, whose loudness follows the music.
        while (sndRunning) {
            MI_AO_ChnState_t st;
            memset(&st, 0, sizeof(st));
            if (MI_AO_QueryChnStat(sndDev, sndChn, &st) != MI_SUCCESS) break;
            MI_U32 total  = st.u32ChnFreeNum + st.u32ChnBusyNum;
            // Keep only ~3 blocks (~70 ms) queued. Filling the 64 KB queue to
            // the brim seemed to make the output overwrite audio not yet
            // played: skipped chunks (music running fast) plus a jump at each
            // skip (the clicks). The SDL2 driver never keeps it that full.
            MI_U32 target = (total > 64) ? frame.u32Len * 3 : 3; // bytes or blocks
            if (logs < 5) {
                fprintf(stderr, "sound: queue free=%u busy=%u\n",
                    (unsigned)st.u32ChnFreeNum, (unsigned)st.u32ChnBusyNum);
                logs++;
            }
            if (st.u32ChnBusyNum <= target) break;
            usleep(2000);
        }

        if (MI_AO_SendFrame(sndDev, sndChn, &frame, 20) != MI_SUCCESS && logs < 10) {
            fprintf(stderr, "sound: SendFrame rejected a block\n");
            logs++;
        }
    }
    return NULL;
}

bool sndInit() {
    MI_AUDIO_Attr_t attr;
    memset(&attr, 0, sizeof(attr));
    attr.eBitwidth      = E_MI_AUDIO_BIT_WIDTH_16;
    attr.eWorkmode      = E_MI_AUDIO_MODE_I2S_MASTER;
    attr.u32FrmNum      = 6;
    attr.u32PtNumPerFrm = SND_SAMPLES;
    attr.u32ChnCnt      = 2;
    attr.eSoundmode     = E_MI_AUDIO_SOUND_MODE_STEREO;
    attr.eSamplerate    = (MI_AUDIO_SampleRate_e)SND_OUT_RATE;

    if (MI_AO_SetPubAttr(sndDev, &attr) != MI_SUCCESS) {
        fprintf(stderr, "sound: MI_AO_SetPubAttr failed\n");
        return false;
    }
    if (MI_AO_GetPubAttr(sndDev, &sndAttr) != MI_SUCCESS) {
        fprintf(stderr, "sound: MI_AO_GetPubAttr failed\n");
        return false;
    }
    if (MI_AO_Enable(sndDev) != MI_SUCCESS) {
        fprintf(stderr, "sound: MI_AO_Enable failed\n");
        return false;
    }
    if (MI_AO_EnableChn(sndDev, sndChn) != MI_SUCCESS) {
        fprintf(stderr, "sound: MI_AO_EnableChn failed\n");
        return false;
    }
    MI_AO_SetVolume(sndDev, 0);

    MI_SYS_ChnPort_t port;
    memset(&port, 0, sizeof(port));
    port.eModId    = E_MI_MODULE_ID_AO;
    port.u32DevId  = sndDev;
    port.u32ChnId  = sndChn;
    port.u32PortId = 0;
    MI_SYS_SetChnOutputPortDepth(&port, 12, 13);

    sndData = new Sound::Frame[SND_SAMPLES];
    memset(sndData, 0, SND_SAMPLES * sizeof(Sound::Frame));

    sndRunning = true;
    pthread_create(&sndThread, NULL, sndLoop, NULL);
    fprintf(stderr, "sound: MI_AO opened at %d Hz stereo (mixer %d Hz)\n", SND_OUT_RATE, SND_RATE);
    return true;
}

void sndFree() {
    if (sndRunning) {
        sndRunning = false;
        pthread_join(sndThread, NULL);
    }
    MI_AO_DisableChn(sndDev, sndChn);
    MI_AO_Disable(sndDev);
    delete[] sndData;
    sndData = NULL;
}

// input
bool osJoyReady(int index) {
    return index == 0;
}

void osJoyVibrate(int index, float L, float R) {
    //
}

JoyKey getJoyKey(int key) {
    switch (key) {
#ifdef __MIYOO__
        case BTN_A      : return jkA;
        case BTN_B      : return jkB;
        case BTN_X      : return jkX;
        case BTN_Y      : return jkY;
        case BTN_L1     : return jkLB;
        case BTN_R1     : return jkRB;
        case BTN_L2     : return jkLT;
        case BTN_R2     : return jkRT;
#else
        case BTN_B      : return jkX;
        case BTN_A      : return jkA;
        case BTN_TA     : return jkRB;
        case BTN_TB     : return jkY;
#endif
        case BTN_START  : return jkStart;
        case BTN_SELECT : return jkSelect;
        case BTN_UP     : return jkUp;
        case BTN_DOWN   : return jkDown;
        case BTN_LEFT   : return jkLeft;
        case BTN_RIGHT  : return jkRight;
        default         : return jkNone;
    }
}

#include <unistd.h>
#include <dirent.h>
#include <strings.h>

// The core tries several candidate paths for some assets and picks the first
// one this hook confirms, so it must return NULL for files that do not exist.
// The SD card's filesystem is case-sensitive here, while the games' own files
// come in mixed case (TR2 has "data/TITLE.tr2" where the core asks for
// "DATA/TITLE.TR2"). Like the nix platform, match names case-insensitively:
// try the exact name first (free for TR1, whose files are all upper case),
// then walk the path one component at a time.
static char osFixBuf[8][256];
static int  osFixIdx = 0;

const char* osFixFileName(const char* name) {
    if (access(name, F_OK) == 0) return name;

    char *out = osFixBuf[osFixIdx = (osFixIdx + 1) & 7];
    out[0] = '\0';

    const char *p = name;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? size_t(slash - p) : strlen(p);
        char part[128];
        if (len == 0 || len >= sizeof(part)) return NULL;
        memcpy(part, p, len);
        part[len] = '\0';

        DIR *dir = opendir(out[0] ? out : ".");
        if (!dir) return NULL;
        char match[256];
        match[0] = '\0';
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcasecmp(entry->d_name, part) == 0) {
                strncpy(match, entry->d_name, sizeof(match) - 1);
                match[sizeof(match) - 1] = '\0';
                break;
            }
        }
        closedir(dir);
        if (!match[0]) return NULL;

        if (strlen(out) + strlen(match) + 2 > sizeof(osFixBuf[0])) return NULL;
        if (out[0]) strcat(out, "/");
        strcat(out, match);

        p = slash ? slash + 1 : p + len;
    }
    return out;
}

uint16 *swBuffer = NULL;

// Widen the rasterizer's RGB565 output into the framebuffer's XRGB8888.
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

// Converts the 16-bit (RGB565) render buffer to the 32-bit framebuffer and
// rotates it 180 degrees (the panel is mounted upside down). NEON handles 8
// pixels per step: reverse them, widen 565 to 888, and store them backwards,
// so pixel i lands on count-1-i. Roughly 3x faster than the scalar loop.
static inline uint32 conv565(uint16 p) {
    uint32 r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static void swPresent(SDL_Surface *surface) {
    if (SDL_MUSTLOCK(surface)) SDL_LockSurface(surface);

    const uint16 *src   = swBuffer;
    const int     count = SCREEN_WIDTH * SCREEN_HEIGHT;

    if (surface->pitch == SCREEN_WIDTH * 4) {
        uint32 *dst = (uint32*)surface->pixels + count;
        int i = 0;
    #if defined(__ARM_NEON) || defined(__ARM_NEON__)
        const uint16x8_t m5 = vdupq_n_u16(31);
        const uint16x8_t m6 = vdupq_n_u16(63);
        const uint8x8_t  a8 = vdup_n_u8(0xFF);
        for (; i + 8 <= count; i += 8) {
            uint16x8_t px = vld1q_u16(src + i);
            px = vrev64q_u16(px);
            px = vcombine_u16(vget_high_u16(px), vget_low_u16(px));
            uint16x8_t r = vandq_u16(vshrq_n_u16(px, 11), m5);
            uint16x8_t g = vandq_u16(vshrq_n_u16(px, 5), m6);
            uint16x8_t b = vandq_u16(px, m5);
            uint8x8x4_t out;
            out.val[0] = vmovn_u16(vorrq_u16(vshlq_n_u16(b, 3), vshrq_n_u16(b, 2)));
            out.val[1] = vmovn_u16(vorrq_u16(vshlq_n_u16(g, 2), vshrq_n_u16(g, 4)));
            out.val[2] = vmovn_u16(vorrq_u16(vshlq_n_u16(r, 3), vshrq_n_u16(r, 2)));
            out.val[3] = a8;
            dst -= 8;
            vst4_u8((uint8_t*)dst, out);
        }
    #endif
        for (; i < count; i++) {
            *--dst = conv565(src[i]);
        }
    } else {
        // generic path for an unexpected pitch
        for (int y = 0; y < SCREEN_HEIGHT; y++) {
            uint32 *row = (uint32*)((uint8*)surface->pixels + (SCREEN_HEIGHT - 1 - y) * surface->pitch);
            for (int x = 0; x < SCREEN_WIDTH; x++) {
                row[SCREEN_WIDTH - 1 - x] = conv565(src[y * SCREEN_WIDTH + x]);
            }
        }
    }

    if (SDL_MUSTLOCK(surface)) SDL_UnlockSurface(surface);
}

// Miyoo Mini Plus buttons -> the core's pad buttons. The core's default
// bindings follow the PS1 layout by position (Xbox names): left = jump,
// bottom = action, right = roll, top = weapon. The bittboy table above was
// written for the original Miyoo, whose keycodes differ, which left most
// buttons on the wrong action and Select mapped to "quit".
static JoyKey miyooMiniKey(SDLKey sym) {
    switch (sym) {
        case SDLK_LALT   : return jkA;      // Y (left)   - Action (swapped with B by preference)
        case SDLK_LCTRL  : return jkX;      // B (bottom) - Jump (swapped with Y by preference)
        case SDLK_SPACE  : return jkB;      // A (right)  - Roll
        case SDLK_LSHIFT : return jkY;      // X (top)    - Draw/holster weapon
        case SDLK_e      : return jkLB;     // L1         - Look
        case SDLK_RETURN : return jkSelect; // Start      - Inventory too: TR1 on OpenLara has no pause,
                                             //              the rings act as one (jkStart only adds a 2nd player)
        case SDLK_RCTRL  : return jkSelect; // Select     - Inventory
        case SDLK_UP     : return jkUp;
        case SDLK_DOWN   : return jkDown;
        default          : return jkNone;
    }
}

// Buttons that share a core input: R1 (walk), and L2/R2, which sidestep the
// way Tomb Raider does it - walk + left/right. Tracked so that releasing one
// never cancels another still held (e.g. R1 held while tapping L2).
static bool miyooR1, miyooL2, miyooR2, miyooLeft, miyooRight;
// Menu is a modifier: Menu+R1 quick save, Menu+L1 quick load (the core's
// own "5"/"9" keys, so its checks apply: no saving in the rings, etc).
static bool miyooMenu, miyooQuickSave, miyooQuickLoad;

static void miyooUpdateShared() {
    Input::setJoyDown(0, jkRB,    miyooR1 || miyooL2 || miyooR2);
    Input::setJoyDown(0, jkLeft,  miyooLeft  || miyooL2);
    Input::setJoyDown(0, jkRight, miyooRight || miyooR2);
}

// Copying the finished frame to the framebuffer (SDL_Flip) takes ~4 ms: the
// framebuffer is uncached memory. Do it on a thread of its own, so it overlaps
// with the next frame's input, game update and vertex work; the main thread
// only waits for it right before it overwrites the surface again.
static pthread_t       flipThread;
static pthread_mutex_t flipMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  flipCond  = PTHREAD_COND_INITIALIZER;
static bool            flipPending = false;
static bool            flipStarted = false;
static SDL_Surface    *flipScreen  = NULL;
static const GAPI::ColorSW *flipSrc = NULL;

static void* flipProc(void *arg) {
    for (;;) {
        pthread_mutex_lock(&flipMutex);
        while (!flipPending) pthread_cond_wait(&flipCond, &flipMutex);
        pthread_mutex_unlock(&flipMutex);

        // copy the finished (already rotated) frame into the screen
        if (SDL_MUSTLOCK(flipScreen)) SDL_LockSurface(flipScreen);
        const GAPI::ColorSW *src = flipSrc;
        if (flipScreen->pitch == SCREEN_WIDTH * 4) {
            memcpy(flipScreen->pixels, src, SCREEN_WIDTH * SCREEN_HEIGHT * 4);
        } else {
            for (int y = 0; y < SCREEN_HEIGHT; y++) {
                memcpy((uint8*)flipScreen->pixels + y * flipScreen->pitch, src + y * SCREEN_WIDTH, SCREEN_WIDTH * 4);
            }
        }
        if (SDL_MUSTLOCK(flipScreen)) SDL_UnlockSurface(flipScreen);
        if (!(flipScreen->flags & SDL_HWSURFACE)) SDL_Flip(flipScreen);

        pthread_mutex_lock(&flipMutex);
        flipPending = false;
        pthread_cond_broadcast(&flipCond);
        pthread_mutex_unlock(&flipMutex);
    }
    return NULL;
}

static void flipWait() {
    pthread_mutex_lock(&flipMutex);
    while (flipPending) pthread_cond_wait(&flipCond, &flipMutex);
    pthread_mutex_unlock(&flipMutex);
}

static void flipStart(const GAPI::ColorSW *buffer) {
    flipWait();   // one copy at a time
    if (!flipStarted) {
        pthread_create(&flipThread, NULL, flipProc, NULL);
        flipStarted = true;
    }
    pthread_mutex_lock(&flipMutex);
    flipSrc     = buffer;
    flipPending = true;
    pthread_cond_broadcast(&flipCond);
    pthread_mutex_unlock(&flipMutex);
}

// the rasterizer hands each finished frame here
static void presentFrame(const GAPI::ColorSW *buffer) {
    flipStart(buffer);
}

// the rasterizer waits for this before writing into the screen surface
static void rasterWaitsForFlip() {
    flipWait();
}

#include "cdextract.h"   // first run: copy DATA/FMV out of GAME.GOG

int main() {
    fallout::installCrashHandler();

    // One binary serves every game: the shortcut tells it which game folder
    // (holding that game's DATA/FMV/music) to run in, so saves, settings and
    // logs also stay with each game.
    {
        const char *gameDir = getenv("OPENLARA_GAME_DIR");
        if (gameDir && *gameDir && chdir(gameDir) != 0) {
            fprintf(stderr, "OpenLara: can't enter game folder \"%s\"\n", gameDir);
        }
    }

    // GOG/Steam Tomb Raider 1: DATA and FMV live inside the GAME.GOG CD image
    CDExtract::extractGameFiles();
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_NOPARACHUTE);
    SDL_ShowCursor(0);
    // Miyoo Mini: the panel's framebuffer is 32bpp. Asking SDL for 16bpp is
    // accepted but never converted on flip, so raw 16-bit pixels land in a
    // 32-bit buffer and the image tears. Take the surface in its native
    // format and convert ourselves: the rasterizer keeps writing 16-bit
    // pixels (what it is fast at) into our buffer, widened once per frame.
    screen = SDL_SetVideoMode(SCREEN_WIDTH, SCREEN_HEIGHT, 32, SDL_HWSURFACE | SDL_NOFRAME);
    fprintf(stderr, "screen: %s%s pitch=%d\n",
        (screen->flags & SDL_HWSURFACE) ? "hardware" : "software",
        (screen->flags & SDL_DOUBLEBUF) ? " double-buffered" : "", screen->pitch);
    if (screen->pitch != SCREEN_WIDTH * 4) {
        fprintf(stderr, "screen: WARNING unexpected pitch, rendering assumes %d\n", SCREEN_WIDTH * 4);
    }
    flipScreen      = screen;
    GAPI::swPresent = presentFrame;
    swBuffer = new uint16[SCREEN_WIDTH * SCREEN_HEIGHT];

    timeval t;
    gettimeofday(&t, NULL);
    startTime = t.tv_sec;

    Core::width  = SCREEN_WIDTH;
    Core::height = SCREEN_HEIGHT;

    Core::defLang = 0;

    Game::init((const char *)NULL);

    GAPI::resize();
    fprintf(stderr, "DBG surface: %dx%d bpp=%d pitch=%d (esperado pitch=%d)\n",
        screen->w, screen->h, screen->format->BitsPerPixel, screen->pitch,
        screen->w * screen->format->BytesPerPixel); fflush(stderr);

    sndInit();

    bool isQuit = false;

    // The core asks to quit by setting Core::isQuit (passport -> Exit Game),
    // which this loop never checked, so that option did nothing.
    while (!isQuit && !Core::isQuit) {

        SDL_Event event;
        if (SDL_PollEvent(&event)) {

            if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
                const bool down = (event.type == SDL_KEYDOWN);
                const SDLKey sym = event.key.keysym.sym;
                switch (sym) {
                    // Menu key quits on release, so the OnionOS Menu+Power
                    // screenshot combo does not close the game first.
                    // Quitting is left to the passport's "Exit Game".
                    case SDLK_ESCAPE    : miyooMenu = down; break;
                    case SDLK_t         : // R1 - Walk, or quick save with Menu
                        if (down && miyooMenu) {
                            Input::down[ik5] = true;
                            miyooQuickSave = true;
                        } else if (!down && miyooQuickSave) {
                            Input::down[ik5] = false;
                            miyooQuickSave = false;
                        } else {
                            miyooR1 = down;
                            miyooUpdateShared();
                        }
                        break;
                    case SDLK_e         : // L1 - Look, or quick load with Menu
                        if (down && miyooMenu) {
                            Input::down[ik9] = true;
                            miyooQuickLoad = true;
                        } else if (!down && miyooQuickLoad) {
                            Input::down[ik9] = false;
                            miyooQuickLoad = false;
                        } else {
                            Input::setJoyDown(0, jkLB, down);
                        }
                        break;
                    case SDLK_TAB       : miyooL2    = down; miyooUpdateShared(); break; // L2 - Sidestep left
                    case SDLK_BACKSPACE : miyooR2    = down; miyooUpdateShared(); break; // R2 - Sidestep right
                    case SDLK_LEFT      : miyooLeft  = down; miyooUpdateShared(); break;
                    case SDLK_RIGHT     : miyooRight = down; miyooUpdateShared(); break;
                    default             : Input::setJoyDown(0, miyooMiniKey(sym), down); break;
                }
            }
        } else {
            if (Game::update()) {
                Game::render();
                // Rasterize the recorded frame on both cores; each core then
                // converts its own bands straight into the screen surface.
                // Finish the previous frame (the main core's share) and send it
                // to the screen; the worker core starts on this one right away.
                GAPI::swFrameEnd();

                // The panel refreshes at 60 Hz, so anything faster only burns
                // battery and heat. Wait out the rest of a ~16 ms frame.
                {
                    static Uint32 lastFrame = 0;
                    Uint32 elapsed = SDL_GetTicks() - lastFrame;
                    if (lastFrame && elapsed < 16) {
                        SDL_Delay(16 - elapsed);
                    }
                    lastFrame = SDL_GetTicks();
                }
            }
        }

    }

    Game::deinit();

    sndFree();

    // Without this, SDL 1.2's fbcon driver left the console in graphics
    // and raw keyboard mode after quitting, which looked like a freeze.
    GAPI::swDrain();
    flipWait();
    SDL_Quit();

    return 0;
}
