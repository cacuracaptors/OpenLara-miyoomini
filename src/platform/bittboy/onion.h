// OnionOS: the Game Switcher and the save at power-off (Miyoo Mini port).
//
// MENU on its own does what OnionOS's Tweaks set for games ("In-game: Menu
// button", config/keymap.json, read as keymon reads it): by default a press
// opens the Game Switcher, holding it (0.7 s) goes back to the menu and a
// double press (within 0.3 s) switches to the previous game. Each one first
// saves the game in a slot of its own (Game::resumeSave: hidden from the
// passport, apart from the quick save) and writes the picture the Game
// Switcher shows. MENU with another key (Menu+R1/L1 quick save/load, OnionOS's
// Menu+Power screenshot, Menu+volume brightness...) does none of them: the
// keys are read straight from /dev/input/event0 for that, POWER included.
//
// Opened again from the Game Switcher (OnionOS leaves "switcher" in
// /tmp/prev_state), the game goes straight on from that slot; opened from the
// game list, it starts as always.
//
// Power-off: OnionOS creates /tmp/.offOrder and sends SIGTERM (the signal does
// not always arrive, so the file is checked 4 times a second). The game saves
// the same way, leaves resume_boot in its folder and puts back cmd_to_run.sh
// (which keymon removes), so OnionOS opens the game again at the next start
// and it goes on from there. A script in .tmp_update/checkoff makes OnionOS
// wait for the save (20 s at most). Where the game cannot be saved (the title,
// a cutscene, the inventory open...) it just closes, and starts as always.
//
// The Game Switcher's picture: /mnt/SDCARD/Saves/CurrentProfile/romScreens/
// <hash>.png, the hash being OnionOS's FNV1A_Pippip_Yurii of the game's path in
// the recent list (the .port shortcut), as it writes it for RetroArch.
// The same as the Mario Kart 64 port (port/savestate_miyoo.c).
#ifndef H_ONION
#define H_ONION

#include <signal.h>
#include <sys/stat.h>
#include <linux/input.h>

namespace Onion {

    #define ON_DIR        "/mnt/SDCARD/.tmp_update"
    #define ON_CMD        ON_DIR "/cmd_to_run.sh"
    #define ON_SWITCHER   ON_DIR "/.runGameSwitcher"
    #define ON_CHECKOFF   ON_DIR "/checkoff"
    #define ON_SCRIPT     ON_CHECKOFF "/openlara_save.sh"
    #define ON_KEYMAP     ON_DIR "/config/keymap.json"
    #define ON_ROMSCREENS "/mnt/SDCARD/Saves/CurrentProfile/romScreens"
    #define ON_RECENT     "/mnt/SDCARD/Roms/recentlist.json"
    #define ON_RECENT_HID "/mnt/SDCARD/Roms/recentlist-hidden.json"
    #define ON_BOOT       "resume_boot"   // in the game's folder: turned off in a game

    enum { EXIT_SWITCHER, EXIT_MENU, EXIT_QUICK_SWITCH, EXIT_POWEROFF };
    enum { MENU_OFF, MENU_SWITCHER, MENU_EXIT, MENU_QUICK_SWITCH, MENU_RA_MENU };   // keymon's actions
    enum { PRESS_SINGLE, PRESS_LONG, PRESS_DOUBLE };

    static bool  onion, offAtStart, checkoff, resumeAtStart;
    static char  cmd[4096];
    static int   cmdLen = -1;
    static char  romscreen[200];
    static int   menuAction[3] = { MENU_SWITCHER, MENU_EXIT, MENU_QUICK_SWITCH };
    static volatile sig_atomic_t offSignal;
    static long long nextOffPoll;

    static bool exists(const char *path) {
        return access(path, F_OK) == 0;
    }

    // the whole file at once: beside the old one, then renamed over it
    static bool writeFile(const char *path, const void *data, size_t len) {
        char tmp[300];
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) return false;
        const uint8 *p = (const uint8*)data;
        size_t left = len;
        while (left) {
            ssize_t w = write(fd, p, left);
            if (w <= 0) { close(fd); remove(tmp); return false; }
            p += w; left -= size_t(w);
        }
        fsync(fd);
        close(fd);
        if (rename(tmp, path) != 0) { remove(tmp); return false; }
        return true;
    }

    static void touch(const char *path) {
        writeFile(path, "", 0);
    }

    // OnionOS's FNV1A_Pippip_Yurii (src/common/utils/hash.h)
    static uint32 hashPath(const char *str, size_t len) {
        const uint32 prime = 591798841u;
        uint64_t h = 14695981039346656037ULL;
        if (len > 8) {
            size_t cycles = ((len - 1) >> 4) + 1;
            const size_t ndhead = len - (cycles << 3);
            for (; cycles--; str += 8) {
                uint64_t a, b;
                memcpy(&a, str, 8);
                memcpy(&b, str + ndhead, 8);
                h = (h ^ a) * prime;
                h = (h ^ b) * prime;
            }
        } else {
            uint64_t a = 0;
            memcpy(&a, str, len);
            h = (h ^ a) * prime;
        }
        const uint32 h32 = uint32(h ^ (h >> 32));
        return h32 ^ (h32 >> 16);
    }

    // ---- the Game Switcher's picture (PNG, stored without compression) ----
    static uint32 crcTable[256];

    static uint32 crc32(uint32 crc, const uint8 *p, size_t n) {
        if (!crcTable[1])
            for (uint32 i = 0; i < 256; i++) {
                uint32 c = i;
                for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                crcTable[i] = c;
            }
        crc = ~crc;
        while (n--) crc = crcTable[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
        return ~crc;
    }

    static uint8* put32(uint8 *p, uint32 v) {
        p[0] = uint8(v >> 24); p[1] = uint8(v >> 16); p[2] = uint8(v >> 8); p[3] = uint8(v);
        return p + 4;
    }

    // the page on screen (32 bit, the panel mounted upside down: it runs backwards)
    static void writeRomscreen() {
        if (!romscreen[0] || !fbVsync || !fbMem) return;
        const long long t0 = GAPI::swPerfNow();
        const int W = SCREEN_WIDTH, H = SCREEN_HEIGHT;
        const size_t stride = size_t(W) * 3 + 1, n = stride * H;
        const size_t blocks = (n + 65534) / 65535;
        const size_t zlen = 2 + n + blocks * 5 + 4;
        uint8 *file = new uint8[8 + 25 + 12 + zlen + 12];
        uint8 *raw = new uint8[n];
        const uint32 *page = (const uint32*)(fbMem + size_t(fbPage) * SCREEN_WIDTH * SCREEN_HEIGHT * 4);   // the page on screen
        for (int y = 0; y < H; y++) {
            uint8 *o = raw + size_t(y) * stride;
            *o++ = 0;   // filter: none
            const uint32 *s = page + size_t(W) * H - 1 - size_t(y) * W;
            for (int x = 0; x < W; x++) {
                const uint32 c = *s--;
                *o++ = uint8(c >> 16); *o++ = uint8(c >> 8); *o++ = uint8(c);
            }
        }
        static const uint8 sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
        uint8 *p = file;
        memcpy(p, sig, 8); p += 8;
        uint8 *c = p;
        p = put32(p, 13); memcpy(p, "IHDR", 4); p += 4;
        p = put32(p, W); p = put32(p, H);
        *p++ = 8; *p++ = 2; *p++ = 0; *p++ = 0; *p++ = 0;
        p = put32(p, crc32(0, c + 4, 17));
        c = p;
        p = put32(p, uint32(zlen)); memcpy(p, "IDAT", 4); p += 4;
        uint8 *z = p;
        *p++ = 0x78; *p++ = 0x01;
        uint32 a = 1, b = 0;
        for (size_t i = 0; i < n; i++) { a = (a + raw[i]) % 65521u; b = (b + a) % 65521u; }
        for (size_t off = 0; off < n;) {
            const size_t len = n - off > 65535 ? 65535 : n - off;
            *p++ = (off + len >= n) ? 1 : 0;
            *p++ = uint8(len); *p++ = uint8(len >> 8);
            *p++ = uint8(~len); *p++ = uint8(~len >> 8);
            memcpy(p, raw + off, len); p += len; off += len;
        }
        p = put32(p, (b << 16) | a);
        p = put32(p, crc32(0, c + 4, size_t(p - z) + 4));
        c = p;
        p = put32(p, 0); memcpy(p, "IEND", 4); p += 4;
        p = put32(p, crc32(0, c + 4, 4));
        mkdir(ON_ROMSCREENS, 0777);
        const bool ok = writeFile(romscreen, file, size_t(p - file));
        delete[] raw;
        delete[] file;
        fprintf(stderr, "onion: imagem do Game Switcher %s (%.0f ms)\n", ok ? "gravada" : "FALHOU", (GAPI::swPerfNow() - t0) / 1000.0);
    }

    // the game's line in OnionOS's recent list: the path the Game Switcher hashes
    static void findRomscreen() {
        char cwd[256];
        if (!getcwd(cwd, sizeof(cwd))) return;
        const char *dir = strrchr(cwd, '/');
        dir = dir ? dir + 1 : cwd;   // the game's folder: "Tomb Raider 1"
        FILE *f = fopen(exists(ON_RECENT_HID) ? ON_RECENT_HID : ON_RECENT, "r");
        if (!f) return;
        static char line[8192];
        char path[512];
        while (fgets(line, sizeof(line), f)) {
            const char *t = strstr(line, "\"type\":");
            const char *r = strstr(line, "\"rompath\":\"");
            if (!t || !r) continue;
            const int type = atoi(t + 7);
            if (type != 5 && type != 17) continue;
            r += 11;
            const char *e = strchr(r, '"');
            if (!e || e - r >= int(sizeof(path))) continue;
            memcpy(path, r, size_t(e - r));
            path[e - r] = 0;
            const char *colon = strchr(path, ':');   // "launch:rompath" (the search panel)
            const char *rom = colon ? colon + 1 : path;
            bool ours = strstr(rom, dir) != NULL;
            if (!ours) {   // the .port shortcut names the folder (GameFolder=".../Tomb Raider 1")
                FILE *s = fopen(rom, "r");
                if (s) {
                    char buf[4096];
                    const size_t k = fread(buf, 1, sizeof(buf) - 1, s);
                    buf[k] = 0;
                    fclose(s);
                    ours = strstr(buf, dir) != NULL;
                }
            }
            if (ours) {
                snprintf(romscreen, sizeof(romscreen), ON_ROMSCREENS "/%u.png", unsigned(hashPath(rom, strlen(rom))));
                fprintf(stderr, "onion: na lista de recentes como %s -> %s\n", rom, romscreen);
                break;
            }
        }
        fclose(f);
    }

    // OnionOS's settings for MENU in a game (keymon: _settings_load_keymap and the old flags)
    static int jsonInt(const char *json, const char *key, int fallback) {
        char k[64];
        snprintf(k, sizeof(k), "\"%s\"", key);
        const char *p = strstr(json, k);
        if (!p || !(p = strchr(p + strlen(k), ':'))) return fallback;
        return atoi(p + 1);
    }

    static void readMenuSettings() {
        FILE *f = fopen(ON_KEYMAP, "r");
        if (f) {
            char buf[2048];
            const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            buf[n] = 0;
            fclose(f);
            menuAction[PRESS_SINGLE] = jsonInt(buf, "ingame_single_press", menuAction[PRESS_SINGLE]);
            menuAction[PRESS_LONG]   = jsonInt(buf, "ingame_long_press",   menuAction[PRESS_LONG]);
            menuAction[PRESS_DOUBLE] = jsonInt(buf, "ingame_double_press", menuAction[PRESS_DOUBLE]);
        }
        if (exists(ON_DIR "/config/.menuInverted")) {
            menuAction[PRESS_SINGLE] = MENU_EXIT;
            menuAction[PRESS_LONG]   = MENU_SWITCHER;
        }
        if (exists(ON_DIR "/config/.noGameSwitcher")) {
            menuAction[PRESS_SINGLE] = MENU_EXIT;
            menuAction[PRESS_LONG]   = MENU_OFF;
        }
        fprintf(stderr, "onion: MENU (Tweaks): toque %d, segurar %d, dois toques %d (1 Game Switcher, 2 menu, 3 jogo anterior)\n",
                menuAction[PRESS_SINGLE], menuAction[PRESS_LONG], menuAction[PRESS_DOUBLE]);
    }

    // the previous game, as keymon's quick switch does it: the second game of the
    // recent list moves to the top, its command goes into cmd_to_run.sh, and
    // /tmp/quick_switch has runtime.sh start it when this game ends
    static bool quickSwitchPrepare() {
        const char *list = exists(ON_RECENT_HID) ? ON_RECENT_HID : ON_RECENT;
        FILE *f = fopen(list, "r");
        if (!f) return false;
        static char lines[64][2048];
        int n = 0, pick = -1, valid = 0;
        static char next[2200];
        next[0] = 0;
        while (n < 64 && fgets(lines[n], sizeof(lines[n]), f)) {
            const char *line = lines[n++];
            if (pick >= 0) continue;
            const char *t = strstr(line, "\"type\":");
            const char *r = strstr(line, "\"rompath\":\"");
            if (!t || !r) continue;
            const int type = atoi(t + 7);
            if (type != 5 && type != 17) continue;
            char rom[1024], launch[1024] = "";
            r += 11;
            const char *e = strchr(r, '"');
            if (!e || e - r >= int(sizeof(rom))) continue;
            memcpy(rom, r, size_t(e - r));
            rom[e - r] = 0;
            char *colon = strchr(rom, ':');
            if (colon) {   // "launch:rompath"
                *colon = 0;
                snprintf(launch, sizeof(launch), "%s", rom);
                memmove(rom, colon + 1, strlen(colon + 1) + 1);
            } else {
                const char *l = strstr(line, "\"launch\":\"");
                const char *le = l ? strchr(l + 10, '"') : NULL;
                if (le && le - (l + 10) < int(sizeof(launch))) {
                    memcpy(launch, l + 10, size_t(le - (l + 10)));
                    launch[le - (l + 10)] = 0;
                }
            }
            if (!exists(rom) || !launch[0] || !exists(launch)) continue;
            if (valid++ == 1) {
                pick = n - 1;
                snprintf(next, sizeof(next), "LD_PRELOAD=/mnt/SDCARD/miyoo/app/../lib/libpadsp.so \"%s\" \"%s\"", launch, rom);
            }
        }
        const bool complete = feof(f) != 0;
        fclose(f);
        if (pick < 0) return false;
        if (pick > 0 && complete) {   // (a list longer than read here is left as it is)
            static char out[64 * 2048];
            size_t len = 0;
            for (int i = -1; i < n; i++) {
                const int k = i < 0 ? pick : i;
                if (i == pick) continue;
                const size_t l = strlen(lines[k]);
                memcpy(out + len, lines[k], l);
                len += l;
            }
            writeFile(list, out, len);
        }
        if (!writeFile(ON_CMD, next, strlen(next))) return false;
        touch("/tmp/quick_switch");
        touch("/tmp/force_auto_load_state");
        fprintf(stderr, "onion: jogo anterior: %s\n", next);
        return true;
    }

    static void putBackCmd() {
        if (cmdLen > 0 && !exists(ON_CMD)) {
            const bool ok = writeFile(ON_CMD, cmd, size_t(cmdLen));
            fprintf(stderr, "onion: cmd_to_run.sh devolvido (%s)\n", ok ? "ok" : "FALHOU");
        }
    }

    static void makeCheckoffScript() {
        char exe[256] = "OpenLara";
        char link[256];
        const ssize_t k = readlink("/proc/self/exe", link, sizeof(link) - 1);
        if (k > 0) {
            link[k] = 0;
            const char *b = strrchr(link, '/');
            snprintf(exe, sizeof(exe), "%s", b ? b + 1 : link);
        }
        mkdir(ON_CHECKOFF, 0777);
        char s[512];
        const int n = snprintf(s, sizeof(s),
            "#!/bin/sh\n"
            "# Tomb Raider (OpenLara): OnionOS waits here (20 s at most) while the game saves at power-off\n"
            "i=0\n"
            "while grep -q '%s' /proc/%d/cmdline 2>/dev/null && [ $i -lt 200 ]; do sleep 0.1; i=$((i+1)); done\n",
            exe, int(getpid()));
        checkoff = writeFile(ON_SCRIPT, s, size_t(n));
        if (checkoff) chmod(ON_SCRIPT, 0777);
    }

    static void onSigterm(int) {
        offSignal = 1;
    }

    // ---- the keys, straight from evdev: MENU's presses ----
    static int  evFd = -1;
    static uint8 keysDown[KEY_MAX / 8 + 1];
    static bool menuDown, menuMixed, menuLongDone, menuSingleWait, menuSkipRelease;
    static long long menuPressT;
    static int  menuPress = -1;
    #define ON_MENU_LONG_US   700000
    #define ON_MENU_DOUBLE_US 300000

    static bool othersDown() {
        for (int i = 0; i < int(sizeof(keysDown)); i++) {
            uint8 m = keysDown[i];
            if (i == KEY_ESC / 8) m &= uint8(~(1u << (KEY_ESC % 8)));
            if (m) return true;
        }
        return false;
    }

    static void pollKeys() {
        if (evFd < 0) return;
        input_event ev[32];
        const long long now = GAPI::swPerfNow();
        for (;;) {
            const ssize_t r = read(evFd, ev, sizeof(ev));
            if (r <= 0) break;
            const int n = int(r / sizeof(ev[0]));
            for (int i = 0; i < n; i++) {
                if (ev[i].type != EV_KEY) continue;
                const int code = ev[i].code;
                if (code >= 0 && code < KEY_MAX) {
                    if (ev[i].value) keysDown[code / 8] |= uint8(1u << (code % 8));
                    else             keysDown[code / 8] &= uint8(~(1u << (code % 8)));
                }
                if (code == KEY_ESC && ev[i].value == 1) {          // MENU pressed
                    if (menuSingleWait && now - menuPressT <= ON_MENU_DOUBLE_US) {
                        menuSingleWait  = false;                    // the second press: double
                        menuSkipRelease = true;
                        menuPress = PRESS_DOUBLE;
                    } else {
                        menuSkipRelease = false;
                    }
                    menuDown     = true;
                    menuMixed    = othersDown();
                    menuLongDone = false;
                    menuPressT   = now;
                } else if (code == KEY_ESC && ev[i].value == 0) {   // MENU released
                    if (menuDown && !menuMixed && !menuLongDone && !menuSkipRelease && !othersDown()) {
                        if (menuAction[PRESS_DOUBLE] != MENU_OFF)
                            menuSingleWait = true;                  // single, unless a second press comes
                        else
                            menuPress = PRESS_SINGLE;
                    }
                    menuDown = false;
                    menuSkipRelease = false;
                } else if (code != KEY_ESC && ev[i].value == 1 && menuDown) {
                    menuMixed = true;                               // another key with MENU
                }
            }
        }
        if (menuDown && !menuMixed && !menuLongDone && !menuSkipRelease && now - menuPressT >= ON_MENU_LONG_US) {
            menuLongDone = true;                                    // held: long press
            menuPress = PRESS_LONG;
        }
        if (menuSingleWait && now - menuPressT > ON_MENU_DOUBLE_US) {
            menuSingleWait = false;
            menuPress = PRESS_SINGLE;
        }
    }

    // ---- start, every frame, end ----

    // before the game opens: what OnionOS left, and how the game was opened
    static void init() {
        onion = exists(ON_DIR);
        if (!onion) return;
        FILE *f = fopen(ON_CMD, "rb");
        if (f) {
            const size_t n = fread(cmd, 1, sizeof(cmd) - 1, f);
            if (n > 0 && n < sizeof(cmd) - 1) cmdLen = int(n);
            fclose(f);
        }
        offAtStart = exists("/tmp/.offOrder");
        makeCheckoffScript();
        findRomscreen();
        readMenuSettings();
        evFd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = onSigterm;
        sigaction(SIGTERM, &sa, NULL);

        char prev[32] = "";
        f = fopen("/tmp/prev_state", "r");
        if (f) {
            if (fgets(prev, sizeof(prev), f)) prev[strcspn(prev, "\r\n")] = 0;
            fclose(f);
        }
        const bool boot = exists(ON_BOOT);
        remove(ON_BOOT);
        resumeAtStart = boot || !strcmp(prev, "switcher") || !strcmp(prev, "game");
        fprintf(stderr, "onion: aberto por '%s'%s: %s\n", prev[0] ? prev : "?", boot ? " (desligou no jogo)" : "",
                resumeAtStart ? "continuando de onde parou" : "comecando do inicio");
    }

    // once the game is up (the title loaded, the saves read)
    static void start() {
        if (!onion) return;
        if (resumeAtStart && Game::resumeLoad()) {
            fprintf(stderr, "onion: continuando do save do Game Switcher\n");
        } else {
            if (resumeAtStart) fprintf(stderr, "onion: nenhum save do Game Switcher: comecando do inicio\n");
            Game::resumeRemove(true);   // opened from the list: an old one would only be in the way
        }
    }

    static void exitSaving(int kind) {
        static const char *names[] = { "Game Switcher", "menu", "jogo anterior", "desligar" };
        fprintf(stderr, "onion: saindo (%s)\n", names[kind]);
        GAPI::swDrain();
        writeRomscreen();
        const bool saved = Game::resumeSave();
        if (!saved) Game::resumeRemove(true);
        sync();
        fprintf(stderr, "onion: %s\n", saved ? "jogo salvo para continuar" : "aqui nao da para salvar: so fechando");
        if (kind == EXIT_POWEROFF && saved) {
            touch(ON_BOOT);
            putBackCmd();
        }
        if (kind == EXIT_SWITCHER) touch(ON_SWITCHER);
        if (kind == EXIT_QUICK_SWITCH && !quickSwitchPrepare())
            fprintf(stderr, "onion: nenhum jogo anterior na lista: de volta ao menu\n");
        sync();
        Core::quit();
    }

    // the time of day against the game's clock, every 2 s: a jump goes to the log
    static void watchClock() {
        static long long lastMono = 0, lastWall = 0;
        const long long mono = GAPI::swPerfNow();
        if (lastMono && mono - lastMono < 2000000) return;
        timeval t;
        gettimeofday(&t, NULL);
        const long long wall = (long long)t.tv_sec * 1000000 + t.tv_usec;
        if (lastMono) {
            const long long d = (wall - lastWall) - (mono - lastMono);
            if (d > 1000000 || d < -1000000)
                fprintf(stderr, "onion: a hora do sistema mudou %+.1f s com o jogo rodando\n", d / 1000000.0);
        }
        lastMono = mono;
        lastWall = wall;
    }

    // between two frames
    static void frame() {
        if (!onion || Core::isQuit) return;
        watchClock();
        pollKeys();
        bool off = offSignal != 0;
        if (!off && !offAtStart) {
            const long long now = GAPI::swPerfNow();
            if (now >= nextOffPoll) {
                nextOffPoll = now + 250000;
                off = exists("/tmp/.offOrder");
            }
        }
        if (off) {
            exitSaving(EXIT_POWEROFF);
            return;
        }
        if (menuPress >= 0) {
            const int press = menuPress;
            menuPress = -1;
            switch (menuAction[press]) {
                case MENU_SWITCHER     : exitSaving(EXIT_SWITCHER);     break;
                case MENU_EXIT         : exitSaving(EXIT_MENU);         break;
                case MENU_QUICK_SWITCH : exitSaving(EXIT_QUICK_SWITCH); break;
                default : break;   // (off, or RetroArch's own menu: nothing here)
            }
        }
    }

    // the game is closing (also after the passport's Exit Game)
    static void end() {
        if (checkoff) unlink(ON_SCRIPT);
        if (evFd >= 0) close(evFd);
    }
}

#endif
