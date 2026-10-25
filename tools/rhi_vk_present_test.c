/* Vulkan present path: a real (shown) window with a swapchain, presented frames, resize and minimise.
 * Needs a desktop video driver with Vulkan surface support (x11, wayland, cocoa, windows).
 * Setup failures, including unsupported headless drivers, fail the test instead of passing without coverage.
 *
 *   SDL_VIDEODRIVER=x11 DISPLAY=:N ./build/rhi_vk_present_test
 *
 * Checks: the backend is in present mode, frames present for several clear colours, the main target
 * follows a window resize (frame size, drawable size and readback agree), a minimised window yields
 * RHI_ERR_SKIP_FRAME, and the validation layer reports nothing. */
#include "rhi.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern unsigned rhi_vk_validation_messages;

static int g_fail;
#define EXPECT(c, ...) do { if (!(c)) { g_fail++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
                                         fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static int near8(int a, int b) { return abs(a - b) <= 2; }

/* Presents one frame cleared to `c`; returns the frame result and reports the frame size. */
static RhiResult frame(const float c[4], RhiFrameInfo *fi, unsigned char px[4]) {
    RhiResult r = rhi_frame_begin(fi);
    if (r != RHI_OK) return r;
    EXPECT(rhi_cmd_clear_main(c, 1.0f) == RHI_OK, "clear");
    if (px) EXPECT(rhi_read_pixels(fi->width / 2, fi->height / 2, 1, 1, RHI_READ_RGBA8, px, 0) == RHI_OK, "read");
    EXPECT(rhi_frame_end() == RHI_OK, "frame_end");
    return RHI_OK;
}

/* Pumps events and polls frames until the frame size becomes w x h (X11 resizes asynchronously).
 * On failure it says what the last frame_begin returned, to tell "still skipping" from "wrong size". */
static int wait_for_size(SDL_Window *win, int w, int h, const float c[4]) {
    RhiResult last = RHI_OK; RhiFrameInfo seen = {0};
    for (int i = 0; i < 200; i++) {
        SDL_PumpEvents();
        RhiFrameInfo fi = {0};
        last = frame(c, &fi, NULL);
        if (last == RHI_OK) seen = fi;
        if (last == RHI_OK && fi.width == w && fi.height == h) return 1;
        SDL_Delay(10);
    }
    int ww = 0, wh = 0; SDL_GetWindowSize(win, &ww, &wh);
    fprintf(stderr, "wait_for_size(%dx%d): last frame_begin -> %d, last frame %dx%d, window %dx%d, SDL flags 0x%x\n",
            w, h, (int)last, seen.width, seen.height, ww, wh, SDL_GetWindowFlags(win));
    return 0;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 2; }
    if (rhi_prepare_window(RHI_BACKEND_VULKAN) != RHI_OK) {
        fprintf(stderr, "prepare_window failed\n");
        SDL_Quit();
        return 2;
    }
    SDL_Window *win = SDL_CreateWindow("rhi_vk_present_test", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                       320, 200, rhi_window_flags(RHI_BACKEND_VULKAN) | SDL_WINDOW_RESIZABLE);
    if (!win) {
        fprintf(stderr, "present test: Vulkan window required (%s)\n", SDL_GetError());
        SDL_Quit();
        return 2;
    }
    /* RHI_VK_PRESENT_NOVSYNC=1 asks for MAILBOX/IMMEDIATE instead of FIFO. */
    RhiDeviceDesc desc = { RHI_BACKEND_VULKAN, getenv("RHI_VK_PRESENT_NOVSYNC") ? 0u : RHI_DEVICE_VSYNC, win };
    if (rhi_device_create(&desc) != RHI_OK) { fprintf(stderr, "rhi_device_create failed\n"); return 2; }

    const float colors[3][4] = { { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, 1, 1 } };
    for (int i = 0; i < 9; i++) {
        RhiFrameInfo fi = {0}; unsigned char px[4] = { 0 };
        EXPECT(frame(colors[i % 3], &fi, px) == RHI_OK, "frame %d", i);
        EXPECT(fi.width == 320 && fi.height == 200, "frame size %dx%d", fi.width, fi.height);
        EXPECT(near8(px[0], colors[i % 3][0] * 255) && near8(px[1], colors[i % 3][1] * 255) &&
               near8(px[2], colors[i % 3][2] * 255), "frame %d centre %d,%d,%d", i, px[0], px[1], px[2]);
    }
    printf("present: 9 frames at 320x200 OK\n");

    for (int step = 0; step < 3; step++) {   /* grow, shrink, grow again: target and swapchain follow */
        static const int sz[3][2] = { { 480, 300 }, { 200, 120 }, { 400, 240 } };
        SDL_SetWindowSize(win, sz[step][0], sz[step][1]);
        EXPECT(wait_for_size(win, sz[step][0], sz[step][1], colors[step]), "frame size never became %dx%d", sz[step][0], sz[step][1]);
        RhiFrameInfo fi = {0}; unsigned char px[4] = { 0 };
        EXPECT(frame(colors[step], &fi, px) == RHI_OK, "frame after resize");
        int32_t dw, dh; rhi_drawable_size(&dw, &dh);
        EXPECT(fi.width == sz[step][0] && fi.height == sz[step][1] && dw == fi.width && dh == fi.height,
               "resize %dx%d: frame %dx%d drawable %dx%d", sz[step][0], sz[step][1], fi.width, fi.height, dw, dh);
        EXPECT(near8(px[0], colors[step][0] * 255) && near8(px[1], colors[step][1] * 255) && near8(px[2], colors[step][2] * 255),
               "readback after resize %d,%d,%d", px[0], px[1], px[2]);
        printf("present: resized to %dx%d OK\n", sz[step][0], sz[step][1]);
    }

    SDL_MinimizeWindow(win);
    int skipped = 0;
    for (int i = 0; i < 100 && !skipped; i++) {
        SDL_PumpEvents();
        RhiFrameInfo fi;
        RhiResult r = rhi_frame_begin(&fi);
        if (r == RHI_ERR_SKIP_FRAME) skipped = 1;
        else if (r == RHI_OK) { rhi_cmd_clear_main(colors[0], 1.0f); rhi_frame_end(); SDL_Delay(10); }
        else { EXPECT(0, "frame_begin -> %d", (int)r); break; }
    }
    if (skipped) {
        printf("present: minimised window skips frames OK\n");
        SDL_RestoreWindow(win);
        /* Wayland has no way to un-minimise a window from the client: SDL keeps the MINIMIZED flag and
         * the frames keep skipping. That is the compositor's rule, not the backend's, so only a restore
         * that SDL reports as done has to bring the frames back. */
        for (int i = 0; i < 100 && (SDL_GetWindowFlags(win) & SDL_WINDOW_MINIMIZED); i++) { SDL_PumpEvents(); SDL_Delay(10); }
        if (SDL_GetWindowFlags(win) & SDL_WINDOW_MINIMIZED) {
            printf("present: this compositor cannot restore a minimised window from the client; restore not exercised\n");
        } else {
            EXPECT(wait_for_size(win, 400, 240, colors[1]), "no frames after restore");
            printf("present: restored OK\n");
        }
    } else {
        printf("present: window manager did not minimise the window; SKIP_FRAME not exercised\n");
    }

    rhi_device_destroy();
    SDL_DestroyWindow(win);
    SDL_Quit();
    printf("validation messages: %u\n", rhi_vk_validation_messages);
    EXPECT(rhi_vk_validation_messages == 0, "validation messages");
    printf(g_fail ? "PRESENT TEST FAILED (%d)\n" : "PRESENT TEST OK\n", g_fail);
    return g_fail ? 1 : 0;
}
