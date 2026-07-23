/* PRetroCalc OS - games: Snake, Breakout, Space Invaders, Game of Life */
#include "apps.h"
#include "gfx.h"
#include "keyboard.h"
#include "sound.h"
#include "os.h"
#include "board.h"
#include "pico/stdlib.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static bool poll_key(int *code, int wait_ms) {
    kbd_event_t ev;
    absolute_time_t until = make_timeout_time_ms(wait_ms);
    while (!time_reached(until)) {
        kbd_poll(); sound_update();
        if (kbd_get_event(&ev) && ev.type == KBD_EV_PRESS) { *code = ev.code; return true; }
        sleep_ms(2);
    }
    return false;
}

static bool dir_key(int c, int *dx, int *dy) {
    switch (c) {
        case KEY_UP: case 'w': case 'W': *dx = 0; *dy = -1; return true;
        case KEY_DOWN: case 's': case 'S': *dx = 0; *dy = 1; return true;
        case KEY_LEFT: case 'a': case 'A': *dx = -1; *dy = 0; return true;
        case KEY_RIGHT: case 'd': case 'D': *dx = 1; *dy = 0; return true;
    }
    return false;
}

/* GEM window + black playfield inside; returns playfield origin */
static int pw_x, pw_y;
static void game_window(const char *title) {
    int cx, cy, cw, ch;
    os_gem_desktop_bg();
    os_window(title, &cx, &cy, &cw, &ch);
    gfx_fill_rect(cx, cy, cw, ch, COL_BLACK);
    pw_x = cx; pw_y = cy;
}

static void game_over(const char *msg, int score) {
    char buf[40];
    /* centered dialog inside the window */
    int dx = pw_x + 20, dy = pw_y + 100, dw = LCD_WIDTH - 2 * dx, dh = 60;
    gfx_fill_rect(dx, dy, dw, dh, COL_BLACK);
    gfx_rect(dx, dy, dw, dh, COL_RED);
    gfx_puts_at(dx + 20, dy + 10, "GAME OVER", COL_RED, COL_BLACK);
    snprintf(buf, sizeof buf, "%s  SCORE:%d", msg, score);
    gfx_puts_at(dx + 12, dy + 28, buf, COL_WHITE, COL_BLACK);
    gfx_puts_at(dx + 12, dy + 44, "ENTER=again  ESC=menu", COL_GRAY, COL_BLACK);
    gfx_flush();
}

/* ================= SNAKE ================= */

#define SN_CELL 8
/* playfield is the GEM window client area (~296x270); use a centered grid */
#define SN_COLS 34
#define SN_ROWS 30
#define SN_MAX 256

void app_snake(void) {
restart:;
    int sx[SN_MAX], sy[SN_MAX];
    int len = 4, dx = 1, dy = 0;
    for (int i = 0; i < len; i++) { sx[i] = 8 - i; sy[i] = 8; }
    int ax = 16, ay = 8, score = 0;
    int speed = 130;
    game_window("SNAKE");
    /* grid origin: centered in client area */
    int gox = pw_x + 6, goy = pw_y + 14;
    for (;;) {
        gfx_fill_rect(pw_x, pw_y, LCD_WIDTH - 2 * pw_x, LCD_HEIGHT - pw_y - 10, COL_BLACK);
        /* playfield border */
        gfx_rect(gox - 2, goy - 2, SN_COLS * SN_CELL + 4, SN_ROWS * SN_CELL + 4, COL_DKGRAY);
        /* food */
        gfx_fill_rect(gox + ax * SN_CELL + 1, goy + ay * SN_CELL + 1, SN_CELL - 2, SN_CELL - 2, COL_RED);
        /* snake */
        for (int i = 0; i < len; i++)
            gfx_fill_rect(gox + sx[i] * SN_CELL + 1, goy + sy[i] * SN_CELL + 1, SN_CELL - 2, SN_CELL - 2,
                          i == 0 ? COL_LGREEN : COL_GREEN);
        char b[24]; snprintf(b, sizeof b, "SCORE %d", score);
        gfx_puts_at(pw_x + 4, pw_y + 2, b, COL_WHITE, COL_BLACK);
        gfx_flush();

        int c;
        if (poll_key(&c, speed)) {
            if (c == KEY_ESC) return;
            int ndx, ndy;
            if (dir_key(c, &ndx, &ndy) && !(ndx == -dx && ndy == -dy)) { dx = ndx; dy = ndy; }
        }
        int nx = sx[0] + dx, ny = sy[0] + dy;
        if (nx < 0 || ny < 0 || nx >= SN_COLS || ny >= SN_ROWS) { sound_beep(200, 300); break; }
        for (int i = 0; i < len; i++) if (sx[i] == nx && sy[i] == ny) { sound_beep(200, 300); goto over; }
        if (nx == ax && ny == ay) {
            score++; sound_beep(1200, 40);
            if (len < SN_MAX) len++;
            if (speed > 60) speed -= 4;
            /* respawn food not on the snake */
            int on;
            do {
                ax = rand() % SN_COLS; ay = rand() % SN_ROWS;
                on = 0;
                for (int i = 0; i < len; i++) if (sx[i] == ax && sy[i] == ay) { on = 1; break; }
            } while (on);
        }
        for (int i = len - 1; i > 0; i--) { sx[i] = sx[i-1]; sy[i] = sy[i-1]; }
        sx[0] = nx; sy[0] = ny;
    }
over:
    game_over("SNAKE", score);
    int c; poll_key(&c, 100000);
    if (c == KEY_ENTER) goto restart;
}

/* ================= BREAKOUT ================= */

void app_breakout(void) {
restart:;
    const int rows = 5, cols = 9;
    uint8_t bricks[5][9];
    memset(bricks, 1, sizeof bricks);
    game_window("BREAKOUT");
    /* playfield inside client area */
    const int fx = pw_x + 6, fy = pw_y + 12;          /* field origin */
    const int fw = LCD_WIDTH - 2 * fx;                 /* field width ~284 */
    const int fb = LCD_HEIGHT - 12;                    /* field bottom */
    const int bw = fw / cols;                          /* brick width */
    int paddle_w = 44, px = fx + (fw - paddle_w) / 2;
    int bx = fx + fw/2, by = fb - 60, bdx = 2, bdy = -3;
    int score = 0, lives = 3;
    const uint8_t rowcol[5] = {COL_RED, COL_ORANGE, COL_YELLOW, COL_GREEN, COL_CYAN};

    for (;;) {
        gfx_fill_rect(pw_x, pw_y, LCD_WIDTH - 2 * pw_x, LCD_HEIGHT - pw_y - 10, COL_BLACK);
        for (int r = 0; r < rows; r++)
            for (int c = 0; c < cols; c++)
                if (bricks[r][c])
                    gfx_fill_rect(fx + c * bw + 1, fy + r * 11, bw - 2, 9, rowcol[r]);
        gfx_fill_rect(px, fb - 8, paddle_w, 5, COL_WHITE);
        gfx_fill_circle(bx, by, 3, COL_YELLOW);
        char b[24]; snprintf(b, sizeof b, "SCORE %d  LIVES %d", score, lives);
        gfx_puts_at(pw_x + 4, pw_y + 2, b, COL_WHITE, COL_BLACK);
        gfx_flush();

        /* input: drain queue quickly, no per-event sleeps that stall the frame */
        int c;
        poll_key(&c, 8);
        if (c == KEY_ESC) return;
        if (c == KEY_LEFT || c == 'a') { px -= 10; if (px < fx) px = fx; }
        if (c == KEY_RIGHT || c == 'd') { px += 10; if (px > fx + fw - paddle_w) px = fx + fw - paddle_w; }
        int c2;
        while (poll_key(&c2, 0)) {
            if (c2 == KEY_LEFT) { px -= 10; if (px < fx) px = fx; }
            else if (c2 == KEY_RIGHT) { px += 10; if (px > fx + fw - paddle_w) px = fx + fw - paddle_w; }
            else if (c2 == KEY_ESC) return;
        }

        bx += bdx; by += bdy;
        if (bx < fx + 3) { bx = fx + 3; bdx = -bdx; sound_beep(800, 10); }
        if (bx > fx + fw - 3) { bx = fx + fw - 3; bdx = -bdx; sound_beep(800, 10); }
        if (by < fy + 2) { by = fy + 2; bdy = -bdy; sound_beep(800, 10); }
        /* paddle: only bounce when moving down and crossing the paddle plane */
        if (bdy > 0 && by >= fb - 12 && by <= fb - 2 && bx >= px - 2 && bx <= px + paddle_w + 2) {
            by = fb - 12;                 /* clamp out of the paddle */
            bdy = -bdy;
            /* english: offset from paddle centre sets horizontal speed.
             * left half -> negative (left), right half -> positive (right). */
            bdx = (bx - (px + paddle_w / 2)) / 3;
            if (bdx > 6) bdx = 6;
            if (bdx < -6) bdx = -6;
            sound_beep(1000, 15);
        }
        if (by > fb + 4) {
            lives--; sound_beep(200, 250);
            if (lives == 0) break;
            bx = fx + fw/2; by = fb - 60; bdx = 2; bdy = -3;
        }
        /* brick collision (ball moving up) */
        if (bdy < 0 && by >= fy && by < fy + rows * 11) {
            int r = (by - fy) / 11, cc = (bx - fx) / bw;
            if (r >= 0 && r < rows && cc >= 0 && cc < cols && bricks[r][cc]) {
                bricks[r][cc] = 0; bdy = -bdy; score += 10; sound_beep(1400, 15);
            }
        }
        int left = 0;
        for (int r = 0; r < rows; r++) for (int c3 = 0; c3 < cols; c3++) left += bricks[r][c3];
        if (left == 0) {
            gfx_puts_at(fx + fw/2 - 32, fy + 80, "YOU WIN!", COL_LGREEN, COL_BLACK);
            gfx_flush();
            poll_key(&c, 100000);
            if (c == KEY_ESC) return;
            goto restart;
        }
    }
    game_over("BREAKOUT", score);
    int c; poll_key(&c, 100000);
    if (c == KEY_ENTER) goto restart;
}

/* ================= SPACE INVADERS ================= */

#define INV_ROWS 4
#define INV_COLS 8

void app_invaders(void) {
restart:;
    uint8_t inv[INV_ROWS][INV_COLS];
    memset(inv, 1, sizeof inv);
    game_window("INVADERS");
    const int fx = pw_x + 6, fy = pw_y + 12;
    const int fw = LCD_WIDTH - 2 * fx;
    const int fbot = LCD_HEIGHT - 12;
    int inv_x = fx + 10, inv_y = fy + 8, inv_dx = 2;
    int ship_x = fx + fw / 2;
    int bul_x = -1, bul_y = -1;
    int ebul_x = -1, ebul_y = -1;
    int score = 0, lives = 3;

    for (;;) {
        gfx_fill_rect(pw_x, pw_y, LCD_WIDTH - 2 * pw_x, LCD_HEIGHT - pw_y - 10, COL_BLACK);
        /* invaders */
        int alive = 0;
        for (int r = 0; r < INV_ROWS; r++)
            for (int c = 0; c < INV_COLS; c++)
                if (inv[r][c]) {
                    int x = inv_x + c * 32, y = inv_y + r * 22;
                    gfx_fill_rect(x + 2, y + 4, 20, 10, COL_MAGENTA);
                    gfx_fill_rect(x, y + 8, 24, 4, COL_MAGENTA);
                    alive++;
                }
        /* ship + bullets */
        gfx_fill_rect(ship_x - 12, fbot - 14, 24, 6, COL_LGREEN);
        gfx_fill_rect(ship_x - 2, fbot - 20, 4, 8, COL_LGREEN);
        if (bul_y >= 0) gfx_fill_rect(bul_x - 1, bul_y, 2, 8, COL_YELLOW);
        if (ebul_y >= 0) gfx_fill_rect(ebul_x - 1, ebul_y, 2, 8, COL_RED);
        char b[32]; snprintf(b, sizeof b, "SCORE %d  LIVES %d", score, lives);
        gfx_puts_at(pw_x + 4, pw_y + 2, b, COL_WHITE, COL_BLACK);
        gfx_flush();

        int c;
        poll_key(&c, 30);
        if (c == KEY_ESC) return;
        if (c == KEY_LEFT) { ship_x -= 6; if (ship_x < fx + 14) ship_x = fx + 14; }
        if (c == KEY_RIGHT) { ship_x += 6; if (ship_x > fx + fw - 14) ship_x = fx + fw - 14; }
        if ((c == ' ' || c == KEY_ENTER) && bul_y < 0) { bul_x = ship_x; bul_y = fbot - 24; sound_beep(1600, 20); }

        /* move swarm */
        inv_x += inv_dx;
        if (inv_x < fx + 4 || inv_x + INV_COLS * 32 > fx + fw - 8) {
            inv_dx = -inv_dx;
            inv_y += 10;
            if (inv_y + INV_ROWS * 22 > fbot - 40) break; /* invaded */
        }
        /* enemy fire */
        if (ebul_y < 0 && (rand() % 30) == 0 && alive > 0) {
            int r, cc, tries = 20;
            do { r = rand() % INV_ROWS; cc = rand() % INV_COLS; } while (!inv[r][cc] && --tries);
            ebul_x = inv_x + cc * 32 + 12; ebul_y = inv_y + r * 22 + 16;
        }
        if (bul_y >= 0) {
            bul_y -= 6;
            if (bul_y < fy) bul_y = -1;
            else {
                for (int r = 0; r < INV_ROWS && bul_y >= 0; r++)
                    for (int c2 = 0; c2 < INV_COLS; c2++)
                        if (inv[r][c2]) {
                            int x = inv_x + c2 * 32, y = inv_y + r * 22;
                            if (bul_x >= x && bul_x <= x + 24 && bul_y >= y && bul_y <= y + 14) {
                                inv[r][c2] = 0; bul_y = -1; score += 10; sound_beep(600, 30);
                            }
                        }
            }
        }
        if (ebul_y >= 0) {
            ebul_y += 5;
            if (ebul_y > fbot) ebul_y = -1;
            else if (ebul_y > fbot - 22 && ebul_x > ship_x - 14 && ebul_x < ship_x + 14) {
                ebul_y = -1; lives--; sound_beep(180, 300);
                if (lives == 0) break;
            }
        }
        if (alive == 0) { score += 100; goto restart; }
    }
    game_over("INVADERS", score);
    int c; poll_key(&c, 100000);
    if (c == KEY_ENTER) goto restart;
}

/* ================= GAME OF LIFE ================= */

#define LIFE_CELL 4
#define LIFE_W 72
#define LIFE_H 64

static uint8_t life_a[LIFE_W * LIFE_H], life_b[LIFE_W * LIFE_H];

void app_life(void) {
    memset(life_a, 0, sizeof life_a);
    int cx = LIFE_W / 2, cy = LIFE_H / 2;
    life_a[(cy)*LIFE_W + cx+1] = 1; life_a[(cy)*LIFE_W + cx+2] = 1;
    life_a[(cy+1)*LIFE_W + cx] = 1; life_a[(cy+1)*LIFE_W + cx+1] = 1;
    life_a[(cy+2)*LIFE_W + cx+1] = 1;
    life_a[10*LIFE_W+10]=1; life_a[11*LIFE_W+11]=1; life_a[12*LIFE_W+9]=1;
    life_a[12*LIFE_W+10]=1; life_a[12*LIFE_W+11]=1;
    bool running = true;
    int gen = 0;
    game_window("LIFE");
    const int gox = pw_x + 4, goy = pw_y + 12;
    for (;;) {
        gfx_fill_rect(pw_x, pw_y, LCD_WIDTH - 2 * pw_x, LCD_HEIGHT - pw_y - 10, COL_BLACK);
        for (int y = 0; y < LIFE_H; y++)
            for (int x = 0; x < LIFE_W; x++)
                if (life_a[y*LIFE_W+x])
                    gfx_fill_rect(gox + x*LIFE_CELL, goy + y*LIFE_CELL, LIFE_CELL-1, LIFE_CELL-1, COL_GREEN);
        char b[32]; snprintf(b, sizeof b, "GEN %d  %s", gen, running ? "RUN " : "STOP");
        gfx_puts_at(pw_x + 4, pw_y + 2, b, COL_WHITE, COL_BLACK);
        gfx_puts_at(pw_x + 4, LCD_HEIGHT - 12, "SPACE=run/stop R=rand ESC=exit", COL_GRAY, COL_BLACK);
        gfx_flush();

        int c;
        poll_key(&c, running ? 40 : 200);
        if (c == KEY_ESC) return;
        if (c == ' ') running = !running;
        if (c == 'r' || c == 'R') {
            for (int i = 0; i < LIFE_W * LIFE_H; i++) life_a[i] = (rand() % 4 == 0);
            gen = 0;
        }
        if (!running) continue;
        /* step */
        for (int y = 0; y < LIFE_H; y++)
            for (int x = 0; x < LIFE_W; x++) {
                int n = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        if (!dx && !dy) continue;
                        int xx = (x + dx + LIFE_W) % LIFE_W, yy = (y + dy + LIFE_H) % LIFE_H;
                        n += life_a[yy*LIFE_W+xx];
                    }
                uint8_t alive = life_a[y*LIFE_W+x];
                life_b[y*LIFE_W+x] = alive ? (n == 2 || n == 3) : (n == 3);
            }
        memcpy(life_a, life_b, sizeof life_a);
        gen++;
    }
}
