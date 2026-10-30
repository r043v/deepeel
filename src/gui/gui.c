// gui.c: the SDL2 live viewer.  Single-threaded and deterministic: the
// explorer runs in time slices between frames, so a GUI run explores exactly
// like a headless one.  The map shows every bank as a 16x16 grid of 64-byte
// blocks; the panel follows the hover and the selected section.
#include "gui.h"
#include "font5x7.h"
#include "../cpu/sm83/sm83.h"

#include <SDL.h>

enum {
    MS_GW = 1280,
    MS_GH = 860,
    MS_PANEL_W = 400,
    MS_TILE = 16, // blocks per bank tile row/col
};

typedef struct {
    SDL_Window *win;
    SDL_Renderer *ren;
    int block; // pixels per 64-byte block
    int scroll_x, scroll_y;
    int hover_bank, hover_block, has_hover;
    int sel_bank, sel_block, has_sel;
    int finalized;
    char status[128];
} Gui;

static void set_color(SDL_Renderer *ren, uint8_t r, uint8_t g, uint8_t b) {
    SDL_SetRenderDrawColor(ren, r, g, b, 255);
}

static void draw_text(SDL_Renderer *ren, int x, int y, const char *s, int scale, SDL_Color c) {
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 255);
    while (*s) {
        char ch = *s;
        if (ch < 0x20 || ch > 0x7E) ch = '?';
        const uint8_t *gl = ms_font5x7[ch - 0x20];
        for (int cx = 0; cx < 5; cx++)
            for (int cy = 0; cy < 7; cy++)
                if (gl[cx] & (1 << cy)) {
                    SDL_Rect r = { x + cx * scale, y + cy * scale, scale, scale };
                    SDL_RenderFillRect(ren, &r);
                }
        x += 6 * scale;
        s++;
    }
}

enum { MS_C_TITLE, MS_C_TEXT, MS_C_DIM, MS_C_LABEL, MS_C_WARN };
static SDL_Color gui_color(int id) {
    switch (id) {
    case MS_C_TITLE: return (SDL_Color){ 140, 230, 120 };
    case MS_C_TEXT: return (SDL_Color){ 205, 205, 210 };
    case MS_C_DIM: return (SDL_Color){ 150, 150, 158 };
    case MS_C_LABEL: return (SDL_Color){ 150, 200, 255 };
    default: return (SDL_Color){ 230, 210, 120 };
    }
}

static int text_w(const char *s, int scale) { return (int)strlen(s) * 6 * scale; }

// bank tile origin: banks wrap into a grid inside the canvas area
static void bank_origin(const Gui *g, int bank, int *ox, int *oy) {
    int ts = MS_TILE * g->block + 4;
    int per_row = (MS_GW - MS_PANEL_W - 16) / ts;
    if (per_row < 1) per_row = 1;
    *ox = 8 + (bank % per_row) * ts - g->scroll_x;
    *oy = 28 + (bank / per_row) * ts - g->scroll_y;
}

static void block_color(const MsResults *res, int bank, int block, uint8_t *r, uint8_t *gg, uint8_t *b) {
    char cls = ms_block_class(res, bank, block);
    switch (cls) {
    case 'C': *r = 0; *gg = 190; *b = 80; break;
    case 'c': *r = 110; *gg = 140; *b = 30; break;
    case 'V': *r = 0; *gg = 210; *b = 210; break;
    case 'D': *r = 70; *gg = 130; *b = 230; break;
    case 'P': *r = 210; *gg = 190; *b = 40; break;
    case 'p': *r = 58; *gg = 58; *b = 62; break;
    default: *r = 30; *gg = 30; *b = 36; break;
    }
}

static void draw_map(Gui *g, const MsResults *res) {
    SDL_Rect clip = { 0, 0, MS_GW - MS_PANEL_W, MS_GH };
    SDL_RenderSetClipRect(g->ren, &clip);
    for (int bank = 0; bank < res->banks; bank++) {
        int ox, oy;
        bank_origin(g, bank, &ox, &oy);
        if (oy > MS_GH || oy + MS_TILE * g->block < 24) continue;
        if (ox > MS_GW - MS_PANEL_W || ox + MS_TILE * g->block < 0) continue;
        char label[32];
        snprintf(label, sizeof label, "b%d", bank);
        draw_text(g->ren, ox, oy - 10 > 0 ? oy - 10 : 0, label, 1, gui_color(MS_C_LABEL));
        for (int by = 0; by < MS_TILE; by++) {
            for (int bx = 0; bx < MS_TILE; bx++) {
                int block = by * MS_TILE + bx;
                uint8_t r, gg, b;
                block_color(res, bank, block, &r, &gg, &b);
                set_color(g->ren, r, gg, b);
                SDL_Rect rect = { ox + bx * g->block, oy + by * g->block, g->block - 1 > 0 ? g->block - 1 : 1,
                                  g->block - 1 > 0 ? g->block - 1 : 1 };
                SDL_RenderFillRect(g->ren, &rect);
            }
        }
        // selection outline
        if (g->has_sel && g->sel_bank == bank) {
            set_color(g->ren, 255, 255, 255);
            int bx = (g->sel_block % MS_TILE) * g->block, by = (g->sel_block / MS_TILE) * g->block;
            SDL_Rect rect = { ox + bx - 1, oy + by - 1, g->block + 1, g->block + 1 };
            SDL_RenderDrawRect(g->ren, &rect);
        }
    }
    SDL_RenderSetClipRect(g->ren, NULL);
}

static void draw_line(SDL_Renderer *ren, int x, int y, int w) {
    set_color(ren, 70, 70, 80);
    SDL_RenderDrawLine(ren, x, y, x + w, y);
}

static const char *verdict_str(uint8_t v) {
    return v == MS_V_REQUIRED ? "required" : (v == MS_V_FORCED ? "forced" : "speculative");
}

static void draw_panel(Gui *g, const MsResults *res, const MsConfig *cfg, const MsProgress *pr) {
    int x = MS_GW - MS_PANEL_W + 8, y = 8, w = MS_PANEL_W - 16;
    char buf[160];
    int sc = 2;

    snprintf(buf, sizeof buf, "memscan - %s (%s)", res->cart->title, ms_mbc_name(res->cart->mbc));
    draw_text(g->ren, x, y, buf, sc, gui_color(MS_C_TEXT));
    y += 20;
    snprintf(buf, sizeof buf, "%d banks  %uKiB rom  %uKiB sram", res->banks, res->cart->rom_size / 1024,
             res->cart->ram_size / 1024);
    draw_text(g->ren, x, y, buf, 1, gui_color(MS_C_TEXT));
    y += 16;

    draw_line(g->ren, x, y, w);
    y += 6;
    snprintf(buf, sizeof buf, "paths %u   steps %llu", pr->paths_run, (unsigned long long)pr->steps_total);
    draw_text(g->ren, x, y, buf, sc, gui_color(MS_C_TEXT));
    y += 18;
    snprintf(buf, sizeof buf, "queue %u   sections %u", pr->queue_len, pr->sections);
    draw_text(g->ren, x, y, buf, sc, gui_color(MS_C_TEXT));
    y += 18;
    snprintf(buf, sizeof buf, "forks %u+%u  evicted %u/%u", res->forks_branch, res->forks_irq,
             res->evicted_exact, res->evicted_ctx);
    draw_text(g->ren, x, y, buf, 1, gui_color(MS_C_TEXT));
    y += 14;
    snprintf(buf, sizeof buf, "cuts cycle %u step %u  outside-rom %u", res->cycle_cuts, res->step_cuts,
             res->exec_outside);
    draw_text(g->ren, x, y, buf, 1, gui_color(MS_C_TEXT));
    y += 16;
    draw_text(g->ren, x, y, g->status, 1, gui_color(MS_C_WARN));
    y += 18;

    draw_line(g->ren, x, y, w);
    y += 6;
    if (g->has_hover) {
        int addr = g->hover_block * 64;
        snprintf(buf, sizeof buf, "hover b%d:%04x", g->hover_bank, addr);
        draw_text(g->ren, x, y, buf, sc, gui_color(MS_C_TEXT));
        y += 18;
        uint8_t r, gg, b;
        block_color(res, g->hover_bank, g->hover_block, &r, &gg, &b);
        SDL_Rect sw = { x + text_w(buf, sc) + 6, y - 14, 12, 12 };
        set_color(g->ren, r, gg, b);
        SDL_RenderFillRect(g->ren, &sw);
    } else {
        draw_text(g->ren, x, y, "hover the map", 1, gui_color(MS_C_TEXT));
        y += 18;
    }
    y += 4;
    draw_line(g->ren, x, y, w);
    y += 6;

    if (g->has_sel) {
        uint32_t key = ms_rom_index(g->sel_bank, (uint16_t)(g->sel_block * 64));
        uint32_t sid1 = key < (uint32_t)res->banks * 0x4000 ? res->section_of[key] : 0;
        if (sid1) {
            const MsSection *s = &res->sections[sid1 - 1];
            snprintf(buf, sizeof buf, "section %d:%04x-%04x", s->bank, s->lo, s->hi);
            draw_text(g->ren, x, y, buf, sc, gui_color(MS_C_TEXT));
            y += 18;
            snprintf(buf, sizeof buf, "hits %u  boot %u  forced %u", s->hits, s->boot_hits, s->forced_hits);
            draw_text(g->ren, x, y, buf, 1, gui_color(MS_C_TEXT));
            y += 14;
            snprintf(buf, sizeof buf, "contexts %u  cond %u/%u both  copies %u", s->ctx_count, s->cond_both,
                     s->cond_sites, s->copies);
            draw_text(g->ren, x, y, buf, 1, gui_color(MS_C_TEXT));
            y += 14;
            snprintf(buf, sizeof buf, "verdict: %s", verdict_str(s->verdict));
            draw_text(g->ren, x, y, buf, sc, gui_color(MS_C_TEXT));
            y += 20;
            if (s->io_used_n) {
                draw_text(g->ren, x, y, "registers:", 1, gui_color(MS_C_TEXT));
                y += 12;
                for (int i = 0; i < s->io_used_n && i < 8; i++) {
                    snprintf(buf, sizeof buf, "  %s", res->plat->io_regs[s->io_used[i]].name);
                    draw_text(g->ren, x, y, buf, 1, gui_color(MS_C_TEXT));
                    y += 12;
                }
            }
            if (s->banks_seen_n) {
                snprintf(buf, sizeof buf, "banks seen:");
                draw_text(g->ren, x, y, buf, 1, gui_color(MS_C_TEXT));
                y += 12;
                int n = s->banks_seen_n < 8 ? s->banks_seen_n : 8;
                for (int i = 0; i < n; i++) {
                    snprintf(buf, sizeof buf, "%u", s->banks_seen[i]);
                    draw_text(g->ren, x + i * 30, y, buf, 1, gui_color(MS_C_TEXT));
                }
                y += 14;
            }
            // disassembly head of the section
            draw_text(g->ren, x, y, "code:", 1, gui_color(MS_C_TEXT));
            y += 12;
            uint16_t a = s->lo;
            for (int i = 0; i < 8 && a <= s->hi; i++) {
                uint32_t off = (uint32_t)s->bank * 0x4000 + (a & 0x3FFF);
                char txt[48];
                int len = off < res->cart->rom_size
                              ? sm83_disasm(res->cart->rom, res->cart->rom_size, (uint16_t)off, txt, sizeof txt)
                              : 0;
                snprintf(buf, sizeof buf, "  %04x %s", a, len ? txt : ".db");
                draw_text(g->ren, x, y, buf, 1, gui_color(MS_C_TEXT));
                y += 12;
                if (len <= 0) break;
                uint16_t next = (uint16_t)(a + len);
                if (next <= a) break;
                a = next;
            }
            y += 4;
        } else {
            int addr = g->sel_block * 64;
            snprintf(buf, sizeof buf, "b%d:%04x  no section", g->sel_bank, addr);
            draw_text(g->ren, x, y, buf, sc, gui_color(MS_C_TEXT));
            y += 20;
        }
    } else {
        draw_text(g->ren, x, y, "click a block for its section", 1, gui_color(MS_C_TEXT));
        y += 20;
    }

    draw_line(g->ren, x, y, w);
    y += 6;
    draw_text(g->ren, x, y, "register usage:", 1, gui_color(MS_C_TEXT));
    y += 12;
    int shown = 0;
    for (int i = 0; i < res->io_count && shown < 8; i++) {
        const MsIoStat *st = &res->io[i];
        if (!st->rd && !st->wr) continue;
        snprintf(buf, sizeof buf, "  %-6s r%-7u w%-7u", res->plat->io_regs[i].name, st->rd, st->wr);
        draw_text(g->ren, x, y, buf, 1, gui_color(MS_C_TEXT));
        y += 12;
        shown++;
    }
    y += 6;
    if (res->copy_count) {
        snprintf(buf, sizeof buf, "copies (%d):", res->copy_count);
        draw_text(g->ren, x, y, buf, 1, gui_color(MS_C_TEXT));
        y += 12;
        for (int i = 0; i < res->copy_count && i < 6; i++) {
            const MsCopyRec *r = &res->copies[i];
            static const char *ch[3] = { "loop", "dma", "hdma" };
            snprintf(buf, sizeof buf, "  %s b%u:%04x>%04x %uB", ch[r->channel], r->src_bank, r->src_lo,
                     r->src_hi, r->bytes);
            draw_text(g->ren, x, y, buf, 1, gui_color(MS_C_TEXT));
            y += 12;
        }
    }
    (void)cfg;
}

static void pick_block(Gui *g, const MsResults *res, int mx, int my, int *bank, int *block) {
    *bank = -1;
    int ts = MS_TILE * g->block + 4;
    int per_row = (MS_GW - MS_PANEL_W - 16) / ts;
    if (per_row < 1) per_row = 1;
    int col = (mx + g->scroll_x - 8) / ts;
    int row = (my + g->scroll_y - 28) / ts;
    int b = row * per_row + col;
    if (b < 0 || b >= res->banks) return;
    int px = (mx + g->scroll_x - 8) - col * ts;
    int py = (my + g->scroll_y - 28) - row * ts;
    if (px >= MS_TILE * g->block || py >= MS_TILE * g->block) return;
    *bank = b;
    *block = (py / g->block) * MS_TILE + (px / g->block);
}

int ms_gui_available(void) { return 1; }

int ms_gui_run(MsResults *res, MsConfig *cfg, MsExplorer *ex) {
    Gui g;
    memset(&g, 0, sizeof g);
    g.block = 4;
    snprintf(g.status, sizeof g.status, "exploring...");

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "memscan: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    g.win = SDL_CreateWindow("memscan", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, MS_GW, MS_GH,
                             SDL_WINDOW_RESIZABLE);
    g.ren = SDL_CreateRenderer(g.win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g.ren) g.ren = SDL_CreateRenderer(g.win, -1, 0);

    int done = 0;
    int frames = 0;
    const char *shot = getenv("MEMSCAN_SHOT"); // headless testing: save a frame and exit
    int shot_frame = shot ? 40 : -1;
    while (!done) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT:
                done = 1;
                break;
            case SDL_KEYDOWN:
                if (e.key.keysym.sym == SDLK_ESCAPE) done = 1;
                break;
            case SDL_MOUSEWHEEL: {
                int old = g.block;
                g.block += e.wheel.y > 0 ? 1 : -1;
                if (g.block < 2) g.block = 2;
                if (g.block > 12) g.block = 12;
                (void)old;
                break;
            }
            case SDL_MOUSEMOTION: {
                int b, blk;
                pick_block(&g, res, e.motion.x, e.motion.y, &b, &blk);
                g.has_hover = b >= 0;
                if (b >= 0) {
                    g.hover_bank = b;
                    g.hover_block = blk;
                }
                if (e.motion.state & SDL_BUTTON_LMASK) {
                    g.scroll_x -= e.motion.xrel;
                    g.scroll_y -= e.motion.yrel;
                    if (g.scroll_x < 0) g.scroll_x = 0;
                    if (g.scroll_y < 0) g.scroll_y = 0;
                }
                break;
            }
            case SDL_MOUSEBUTTONDOWN:
                if (e.button.button == SDL_BUTTON_LEFT) {
                    int b, blk;
                    pick_block(&g, res, e.button.x, e.button.y, &b, &blk);
                    if (b >= 0) {
                        g.has_sel = 1;
                        g.sel_bank = b;
                        g.sel_block = blk;
                    }
                }
                break;
            }
        }
        // exploration slice between frames
        if (!g.finalized) {
            MsProgress pr;
            ms_explorer_progress(ex, &pr);
            if (pr.queue_len == 0) {
                ms_explore_finalize(ex);
                g.finalized = 1;
                snprintf(g.status, sizeof g.status, "done - %llu steps", (unsigned long long)pr.steps_total);
            } else {
                ms_explore_slice(ex, 8);
            }
        }
        MsProgress pr;
        ms_explorer_progress(ex, &pr);
        set_color(g.ren, 16, 16, 20);
        SDL_RenderClear(g.ren);
        draw_map(&g, res);
        draw_panel(&g, res, cfg, &pr);
        SDL_RenderPresent(g.ren);
        if (frames == shot_frame) {
            SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, MS_GW, MS_GH, 24, SDL_PIXELFORMAT_RGB24);
            SDL_RenderReadPixels(g.ren, NULL, SDL_PIXELFORMAT_RGB24, s->pixels, s->pitch);
            SDL_SaveBMP(s, shot);
            SDL_FreeSurface(s);
            done = 1;
        }
        frames++;
        SDL_Delay(12);
    }
    if (!g.finalized) ms_explore_finalize(ex);
    SDL_DestroyRenderer(g.ren);
    SDL_DestroyWindow(g.win);
    SDL_Quit();
    return 0;
}
