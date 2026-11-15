#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <stdarg.h>

// ---------------------------------------------------------------------------
// Display: render at the Switch's native 720p instead of 640x480.
// The old build rendered 640x480 and let the compositor upscale it to 720p,
// which softened both the IR image and the UI text.
// Falls back to smaller framebuffers if the applet memory budget refuses 720p.
// ---------------------------------------------------------------------------
static int g_fb_w = 1280;
static int g_fb_h = 720;

// System tick is 19.2 MHz on the Switch (used for the FPS counter).
#define SYS_TICK_HZ 19200000.0

// Available resolutions for cycling. These are the only formats the right
// Joy-Con IR sensor + IRS service expose: 320x240 is the hardware ceiling.
static IrsImageTransferProcessorFormat formats[] = {
    IrsImageTransferProcessorFormat_20x15,
    IrsImageTransferProcessorFormat_40x30,
    IrsImageTransferProcessorFormat_80x60,
    IrsImageTransferProcessorFormat_160x120,
    IrsImageTransferProcessorFormat_320x240
};
static const int num_formats = sizeof(formats)/sizeof(formats[0]);
static int current_format_index = 3; // start at 160x120

// Toggles
static bool useThermal = false;
static bool ledsEnabled = true;   // IR LED toggle
static bool showUI = true;        // UI visibility toggle
static bool autoGain = true;      // software AGC (auto brightness)

// Exposure / gain presets. Exposure time is the real frame-rate lever:
// the sensor's integration time is part of every frame's duration, so a
// short exposure with more digital gain is faster (and noisier), while a
// long exposure is slower and brighter. Gain is the sensor's digital gain
// multiplier (libnx default: 300us, 8x).
typedef struct {
    const char* name;
    u64 exposure_ns;
    u32 gain;
} ExposurePreset;

static const ExposurePreset presets[] = {
    // Gain is pinned to the libnx default (8). On hardware the 16x and 4x
    // presets left the sensor dead (black screen), so only exposure is varied:
    // it is the parameter that actually decides how long a frame takes.
    { "FAST",    100000ULL,  8 },
    { "NORMAL",  300000ULL,  8 },
    { "BRIGHT", 1000000ULL,  8 },
};
static const int num_presets = sizeof(presets)/sizeof(presets[0]);
static int preset_index = 1; // NORMAL

// Result of the last irsRunImageTransferProcessor, plus a small transient
// status line. When the sensor refuses an exposure/gain combination the call
// fails and no frame ever arrives, which used to mean a permanently black
// screen: now the failure is shown and the previous preset is restored.
static Result last_start_rc = 0;
static char   ir_status_msg[64] = "";
static u64    ir_status_until = 0;
static u64    proc_start_tick = 0;
static u64    last_frame_tick = 0;
static bool   stalled = false;
static bool   frame_seen_since_start = false;
static bool   auto_recover_used = false;
static int    preset_before = 1; // last preset the sensor accepted

static void setStatus(const char* text, double seconds) {
    snprintf(ir_status_msg, sizeof(ir_status_msg), "%s", text);
    ir_status_until = svcGetSystemTick() + (u64)(SYS_TICK_HZ * seconds);
}

// Per-frame pixel cache at native IR resolution (max 320x240) so the scaled
// image can be re-blitted every loop without recomputing the colour mapping.
static u32 ir_rgba[320*240];

// Lookup table: intensity -> RGBA, rebuilt once per new frame.
static u32 lut[256];
static float agc_gain = 1.0f; // smoothed software auto-gain

static u32 ir_w = 0, ir_h = 0; // current native IR dimensions

// FPS counter
static u32 frames_in_window = 0;
static u64 fps_window_start = 0;
static float fps_value = 0.0f;
static u32 ambient_noise = 0;

// Thermal colour mapping
static u32 thermalColor(u8 intensity) {
    float t = intensity / 255.0f;
    u8 r, g, b;

    if (t < 0.25f) {          // black → blue
        r = 0; g = 0; b = (u8)(t / 0.25f * 255);
    } else if (t < 0.5f) {    // blue → green
        r = 0; g = (u8)((t - 0.25f) / 0.25f * 255); b = 255 - g;
    } else if (t < 0.75f) {   // green → yellow
        r = (u8)((t - 0.5f) / 0.25f * 255); g = 255; b = 0;
    } else {                  // yellow → red → white
        // The original ramp went yellow -> magenta (b rose while g fell),
        // so the brightest pixels turned pink instead of white. Split the
        // last quarter: 0.75-0.875 red-ward, 0.875-1.0 to white.
        float u = (t - 0.75f) / 0.25f;
        if (u < 0.5f) {
            r = 255; g = (u8)(255.0f - u * 2.0f * 255.0f); b = 0;
        } else {
            r = 255;
            g = (u8)((u - 0.5f) * 2.0f * 255.0f);
            b = g;
        }
    }
    return RGBA8_MAXALPHA(r, g, b);
}

// --- Font constants and helpers ---
static const int FONT_W = 5;
static const int FONT_H = 7;

typedef struct { char ch; uint8_t rows[7]; } Glyph;

static const Glyph kFont[] = {
    // A–Z
    {'A',{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}}, {'B',{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
    {'C',{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}}, {'D',{0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}},
    {'E',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}}, {'F',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
    {'G',{0x0E,0x11,0x10,0x17,0x11,0x11,0x0E}}, {'H',{0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'I',{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}}, {'J',{0x07,0x02,0x02,0x02,0x12,0x12,0x0C}},
    {'K',{0x11,0x12,0x14,0x18,0x14,0x12,0x11}}, {'L',{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
    {'M',{0x11,0x1B,0x15,0x11,0x11,0x11,0x11}}, {'N',{0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
    {'O',{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}}, {'P',{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
    {'Q',{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}}, {'R',{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
    {'S',{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}}, {'T',{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    {'U',{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}}, {'V',{0x11,0x11,0x11,0x11,0x0A,0x0A,0x04}},
    {'W',{0x11,0x11,0x11,0x11,0x15,0x1B,0x11}}, {'X',{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
    {'Y',{0x11,0x11,0x0A,0x04,0x04,0x04,0x04}}, {'Z',{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},

    // digits
    {'0',{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}}, {'1',{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {'2',{0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}}, {'3',{0x1F,0x01,0x02,0x06,0x01,0x11,0x0E}},
    {'4',{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}}, {'5',{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
    {'6',{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}}, {'7',{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8',{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}}, {'9',{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},

    // symbols
    {' ',{0x00,0x00,0x00,0x00,0x00,0x00,0x00}}, {'-',{0x00,0x00,0x00,0x1F,0x00,0x00,0x00}},
    {'/',{0x01,0x01,0x02,0x04,0x08,0x10,0x10}}, {':',{0x00,0x04,0x04,0x00,0x04,0x04,0x00}},
    {'.',{0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}}, {'%',{0x18,0x19,0x02,0x04,0x08,0x13,0x03}},
    {'|',{0x04,0x04,0x04,0x04,0x04,0x04,0x04}}
};

static const Glyph* findGlyph(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for (size_t i = 0; i < sizeof(kFont)/sizeof(kFont[0]); i++) {
        if (kFont[i].ch == c) return &kFont[i];
    }
    return NULL;
}

static void drawChar(u32* fb, u32 stride_words, int x, int y, char c, u32 color, int scale) {
    const Glyph* g = findGlyph(c);
    if (!g) return;
    for (int ry = 0; ry < FONT_H; ry++) {
        uint8_t row = g->rows[ry];
        for (int rx = 0; rx < FONT_W; rx++) {
            int bit = (row >> (FONT_W - 1 - rx)) & 1;
            if (bit) {
                for (int dy = 0; dy < scale; dy++) {
                    for (int dx = 0; dx < scale; dx++) {
                        int px = x + rx*scale + dx;
                        int py = y + ry*scale + dy;
                        if (px >= 0 && px < g_fb_w && py >= 0 && py < g_fb_h)
                            fb[py * stride_words + px] = color;
                    }
                }
            }
        }
    }
}

static void drawString(u32* fb, u32 stride_words, int x, int y, const char* s, u32 color, int scale, int spacing) {
    int cursor = x;
    while (*s) {
        drawChar(fb, stride_words, cursor, y, *s, color, scale);
        cursor += (FONT_W * scale) + spacing;
        s++;
    }
}

static void drawRect(u32* fb, u32 stride_words, int x, int y, int w, int h, u32 color) {
    for (int yy = 0; yy < h; yy++) {
        int py = y + yy;
        if (py < 0 || py >= g_fb_h) continue;
        int row = py * stride_words;
        for (int xx = 0; xx < w; xx++) {
            int px = x + xx;
            if (px < 0 || px >= g_fb_w) continue;
            fb[row + px] = color;
        }
    }
}

// ---------------------------------------------------------------------------
// Runtime log. Every interesting event goes to two places at once:
//   * a ring buffer drawn full-screen when X is pressed (so a failing config can
//     be read off the console without pulling the SD card), and
//   * sdmc:/switch/IR-Switch.log, flushed on every line so even a crash leaves
//     the last lines behind.
// Keeping the log in RAM first means the overlay still works if the SD card
// mount failed.
// ---------------------------------------------------------------------------
#define LOG_MAX_LINES 192
#define LOG_LINE_LEN  100

static char  log_ring[LOG_MAX_LINES][LOG_LINE_LEN];
static int   log_total = 0;              // lines ever written
static FILE* log_file = NULL;
static bool  log_overlay = false;
static u64   log_tick0 = 0;
static const char* log_path = "sdmc:/switch/IR-Switch.log";

static void logMsg(const char* fmt, ...) {
    char body[LOG_LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    u64 ms = (svcGetSystemTick() - log_tick0) / (u64)(SYS_TICK_HZ / 1000.0);
    char line[LOG_LINE_LEN];
    snprintf(line, sizeof(line), "%5llu.%03llu  %s",
             (unsigned long long)(ms / 1000), (unsigned long long)(ms % 1000), body);

    snprintf(log_ring[log_total % LOG_MAX_LINES], LOG_LINE_LEN, "%s", line);
    log_total++;

    if (log_file) {
        fprintf(log_file, "%s\n", line);
        fflush(log_file);   // survive a crash / power loss
    }
}

static void logInit(void) {
    log_tick0 = svcGetSystemTick();
    Result rc = fsdevMountSdmc();
    if (R_SUCCEEDED(rc)) {
        log_file = fopen(log_path, "w");
        if (!log_file) {
            log_path = "sdmc:/IR-Switch.log";
            log_file = fopen(log_path, "w");
        }
    }
    logMsg("=== IR-Switch 720p + AGC build ===");
    logMsg("sd mount rc 0x%08X, log file: %s", (unsigned)rc,
           log_file ? log_path : "(screen only)");
}

static void drawLogOverlay(u32* fb, u32 stride_words) {
    const int scale = 2;
    const int line_h = FONT_H * scale + 5;
    int max_lines = (g_fb_h - 24) / line_h;
    if (max_lines < 1) max_lines = 1;

    drawRect(fb, stride_words, 0, 0, g_fb_w, g_fb_h, RGBA8_MAXALPHA(0, 0, 0));

    int first = log_total - max_lines;
    if (first < 0) first = 0;
    int y = 12;
    for (int i = first; i < log_total; i++) {
        drawString(fb, stride_words, 8, y, log_ring[i % LOG_MAX_LINES],
                   RGBA8_MAXALPHA(130, 255, 150), scale, 1);
        y += line_h;
    }
    drawString(fb, stride_words, 8, g_fb_h - 16, "X: close log",
               RGBA8_MAXALPHA(180, 180, 180), 1, 1);
}

// ---------------------------------------------------------------------------
// Software auto-gain: find a robust bright reference (98th percentile) and
// stretch the intensity range so the image uses the full palette. This is what
// makes a 320x240, 8-bit IR frame actually readable, and it costs one pass over
// the frame.
// ---------------------------------------------------------------------------
static void updateAGC(const u8* ir) {
    if (!autoGain) { agc_gain = 1.0f; return; }

    u32 hist[256];
    memset(hist, 0, sizeof(hist));
    const u32 count = ir_w * ir_h;
    for (u32 i = 0; i < count; i++) hist[ir[i]]++;

    u32 acc = 0, hi = 255;
    const u32 cutoff = (u32)((float)count * 0.98f);
    for (u32 v = 0; v < 256; v++) {
        acc += hist[v];
        if (acc >= cutoff) { hi = v; break; }
    }
    if (hi < 16) hi = 16;

    float target = 235.0f / (float)hi;
    if (target < 0.6f) target = 0.6f;
    if (target > 6.0f) target = 6.0f;

    // Smooth so the image does not pump frame to frame.
    agc_gain = agc_gain * 0.80f + target * 0.20f;
}

static void buildLUT(void) {
    for (int i = 0; i < 256; i++) {
        float v = (float)i * agc_gain;
        if (v > 255.0f) v = 255.0f;
        u8 b = (u8)v;
        lut[i] = useThermal ? thermalColor(b) : RGBA8_MAXALPHA(b, b, b);
    }
}

static void convertFrame(const u8* ir) {
    const u32 count = ir_w * ir_h;
    updateAGC(ir);
    buildLUT();
    for (u32 i = 0; i < count; i++) ir_rgba[i] = lut[ir[i]];
}

// Integer-scaled, centred blit. Keeps pixels square: no blur, no distortion.
// 240/120/60/30/15 tall formats all map to a 960x720 box at 1280x720.
static void blitScaled(u32* fb, u32 stride_words, const u32* img, u32 iw, u32 ih, int scale, int ox) {
    static u32 rowbuf[1280];
    const u32 row_words = iw * (u32)scale;

    for (u32 y = 0; y < ih; y++) {
        const u32* src = img + y * iw;
        u32* dst = rowbuf;
        for (u32 x = 0; x < iw; x++) {
            u32 c = src[x];
            for (int s = 0; s < scale; s++) *dst++ = c;
        }
        for (int dy = 0; dy < scale; dy++) {
            int py = (int)(y * (u32)scale) + dy;
            if (py >= g_fb_h) break;
            u32* target = fb + (u32)py * stride_words + (u32)ox;
            memcpy(target, rowbuf, row_words * sizeof(u32));
        }
    }
}

// Start image processor with current format, LED state, exposure and gain.
static Result start_ir_processor(IrsIrCameraHandle irhandle,
                                 IrsImageTransferProcessorFormat format) {
    IrsImageTransferProcessorConfig config;
    irsGetDefaultImageTransferProcessorConfig(&config);
    config.format = (u32)format;
    // 0 = all leds on, 1 = bright group, 2 = dim group, 3 = none (off)
    config.light_target = ledsEnabled ? 0 : 3;
    config.exposure_time = presets[preset_index].exposure_ns;
    config.gain = presets[preset_index].gain;
    last_start_rc = irsRunImageTransferProcessor(irhandle, &config, 0x100000);
    logMsg("irs start fmt %d expo %lluus gain %u led %u -> rc 0x%08X",
           (int)format, (unsigned long long)(config.exposure_time / 1000),
           (unsigned)config.gain, (unsigned)config.light_target,
           (unsigned)last_start_rc);
    return last_start_rc;
}

static void resetFrameState(void) {
    agc_gain = 1.0f;
    frames_in_window = 0;
    fps_window_start = svcGetSystemTick();
    fps_value = 0.0f;
    ambient_noise = 0;
    memset(ir_rgba, 0, sizeof(ir_rgba));
}

int main(int argc, char* argv[]) {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    logInit();

    irsInitialize();

    const size_t ir_buffer_size = 0x12c00; // 320*240 bytes
    u8 *ir_buffer = (u8*)malloc(ir_buffer_size);
    if (!ir_buffer) return 0;
    memset(ir_buffer, 0, ir_buffer_size);

    padUpdate(&pad);
    IrsIrCameraHandle irhandle;
    HidNpadIdType idType = padIsHandheld(&pad) ? HidNpadIdType_Handheld : HidNpadIdType_No1;

    Result rc = irsGetIrCameraHandle(&irhandle, idType);
    logMsg("camera handle rc 0x%08X (handheld %d)", (unsigned)rc, padIsHandheld(&pad) != 0);
    if (R_FAILED(rc)) { logMsg("no IR camera, aborting"); free(ir_buffer); return 0; }

    rc = start_ir_processor(irhandle, formats[current_format_index]);
    if (R_FAILED(rc)) {
        logMsg("boot irs start failed 0x%08X, aborting", (unsigned)rc);
        free(ir_buffer);
        return 0;
    }
    proc_start_tick = svcGetSystemTick();

    // Prefer native 720p; degrade if the applet memory budget refuses it.
    Framebuffer fb;
    static const int try_w[] = { 1280, 960, 640 };
    static const int try_h[] = {  720, 480, 480 };
    // framebufferMakeLinear allocates a shadow buffer on top of the N buffers,
    // so 720p double-buffered costs ~3 x 3.7 MB, not 2. If framebufferCreate
    // refuses the size (applet memory budget), retry one step smaller.
    // Its return type changed from void to Result across libnx versions, so the
    // result is deliberately discarded to stay buildable on both.
    bool fb_ok = false;
    for (int i = 0; i < 3; i++) {
        if (R_FAILED(framebufferCreate(&fb, nwindowGetDefault(), try_w[i], try_h[i],
                                       PIXEL_FORMAT_RGBA_8888, 2))) {
            continue;
        }
        framebufferMakeLinear(&fb);
        g_fb_w = try_w[i];
        g_fb_h = try_h[i];
        fb_ok = true;
        break;
    }
    logMsg("framebuffer %dx%d (ok %d)", g_fb_w, g_fb_h, fb_ok != 0);
    if (!fb_ok) {
        logMsg("no usable framebuffer, aborting");
        irsStopImageProcessor(irhandle);
        free(ir_buffer);
        return 0;
    }

    u64 sampling_number = 0;
    resetFrameState();

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);
        if (kDown & HidNpadButton_Plus) break;

        bool reprocess = false;

        // Toggle IR LEDs (apply via light_target)
        if (kDown & HidNpadButton_Minus) {
            ledsEnabled = !ledsEnabled;
            reprocess = true;
        }

        // Toggle UI visibility
        if (kDown & HidNpadButton_StickL) {
            showUI = !showUI;
        }

        // Toggle the log overlay (X)
        if (kDown & HidNpadButton_X) {
            log_overlay = !log_overlay;
            logMsg("log overlay %s", log_overlay ? "on" : "off");
        }

        // Cycle resolutions (restart processor and keep LED state)
        if (kDown & HidNpadButton_Left) {
            if (current_format_index > 0) current_format_index--;
            reprocess = true;
        }
        if (kDown & HidNpadButton_Right) {
            if (current_format_index < num_formats-1) current_format_index++;
            reprocess = true;
        }

        // Exposure / gain presets: the real frame-rate control
        if (kDown & HidNpadButton_ZL) {
            int prev = preset_index;
            if (preset_index > 0) preset_index--;
            if (preset_index != prev) {
                reprocess = true;
                logMsg("preset -> %s (%lluus g%u)", presets[preset_index].name,
                       (unsigned long long)(presets[preset_index].exposure_ns / 1000),
                       (unsigned)presets[preset_index].gain);
            }
        }
        if (kDown & HidNpadButton_ZR) {
            int prev = preset_index;
            if (preset_index < num_presets-1) preset_index++;
            if (preset_index != prev) {
                reprocess = true;
                logMsg("preset -> %s (%lluus g%u)", presets[preset_index].name,
                       (unsigned long long)(presets[preset_index].exposure_ns / 1000),
                       (unsigned)presets[preset_index].gain);
            }
        }

        // Toggle software auto-gain
        if (kDown & HidNpadButton_Y) {
            autoGain = !autoGain;
            reprocess = false;
        }

        // Toggle thermal overlay
        if (kDown & HidNpadButton_Up) useThermal = true;
        if (kDown & HidNpadButton_Down) useThermal = false;

        if (reprocess) {
            logMsg("reprocess fmt %d preset %s", current_format_index,
                   presets[preset_index].name);
            irsStopImageProcessor(irhandle);
            rc = start_ir_processor(irhandle, formats[current_format_index]);
            if (R_FAILED(rc)) {
                // The sensor rejected this exposure/gain. Restore the last one
                // it accepted instead of sitting on a black screen.
                char m[64];
                snprintf(m, sizeof(m), "EXPO REJECTED rc 0x%08X  BACK TO %s",
                         (unsigned)rc, presets[preset_before].name);
                setStatus(m, 5.0);
                logMsg("EXPO REJECTED: %s", m);
                preset_index = preset_before;
                irsStopImageProcessor(irhandle);
                rc = start_ir_processor(irhandle, formats[current_format_index]);
            } else {
                preset_before = preset_index;
            }
            sampling_number = 0;
            resetFrameState();
            proc_start_tick = svcGetSystemTick();
            frame_seen_since_start = false;
        }

        IrsImageTransferProcessorState state;
        rc = irsGetImageTransferProcessorState(irhandle, ir_buffer, ir_buffer_size, &state);

        if (R_SUCCEEDED(rc) && state.sampling_number != sampling_number) {
            sampling_number = state.sampling_number;
            ambient_noise = state.ambient_noise_level;
            frame_seen_since_start = true;

            switch (formats[current_format_index]) {
                case IrsImageTransferProcessorFormat_20x15:   ir_w=20;  ir_h=15;  break;
                case IrsImageTransferProcessorFormat_40x30:   ir_w=40;  ir_h=30;  break;
                case IrsImageTransferProcessorFormat_80x60:   ir_w=80;  ir_h=60;  break;
                case IrsImageTransferProcessorFormat_160x120: ir_w=160; ir_h=120; break;
                case IrsImageTransferProcessorFormat_320x240: ir_w=320; ir_h=240; break;
            }

            convertFrame(ir_buffer);
            frames_in_window++;

            // FPS over a 500 ms window
            static int fps_log_div = 0;
            last_frame_tick = svcGetSystemTick();
            if (stalled) {
                stalled = false;
                logMsg("frames resumed");
            }
            u64 now = svcGetSystemTick();
            u64 elapsed = now - fps_window_start;
            if (elapsed >= (u64)(SYS_TICK_HZ * 0.5)) {
                fps_value = (float)frames_in_window * (float)(SYS_TICK_HZ / (double)elapsed);
                frames_in_window = 0;
                fps_window_start = now;
                // one line per second: the framerate history ends up in the file
                if (++fps_log_div >= 2) {
                    fps_log_div = 0;
                    logMsg("fps %.1f  frame %llu  fmt %d  expo %s  agc %.2f  noise %u",
                           (double)fps_value, (unsigned long long)sampling_number,
                           current_format_index, presets[preset_index].name,
                           (double)agc_gain, ambient_noise);
                }
            }
        }

        // Stall detection: the processor is running but no frame is arriving.
        if (!stalled && last_frame_tick) {
            u64 st = svcGetSystemTick();
            if (st - last_frame_tick > (u64)(SYS_TICK_HZ * 0.7)) {
                stalled = true;
                logMsg("no frames for %llums (fmt %d, expo %s)",
                       (unsigned long long)((st - last_frame_tick) / (u64)(SYS_TICK_HZ / 1000.0)),
                       current_format_index, presets[preset_index].name);
            }
        }

        // Watchdog: some configs are accepted by irsRunImageTransferProcessor but
        // never deliver frames. Give the sensor 1.5 s, then drop back to the
        // known-good preset (once per boot, so it cannot loop).
        if (!frame_seen_since_start && !auto_recover_used && proc_start_tick) {
            u64 wt = svcGetSystemTick();
            if (wt - proc_start_tick > (u64)(SYS_TICK_HZ * 1.5)) {
                char m[64];
                snprintf(m, sizeof(m), "NO FRAME ON %s  REVERT TO %s",
                         presets[preset_index].name, presets[1].name);
                setStatus(m, 6.0);
                logMsg("WATCHDOG: %s", m);
                auto_recover_used = true;
                preset_index = 1;
                irsStopImageProcessor(irhandle);
                start_ir_processor(irhandle, formats[current_format_index]);
                sampling_number = 0;
                resetFrameState();
                proc_start_tick = wt;
            }
        }

        u32 stride;
        u32* framebuf = (u32*)framebufferBegin(&fb, &stride);

        if (framebuf) {
            const u32 stride_words = (stride >> 2);

            if (ir_w && ir_h) {
                int scale = g_fb_h / (int)ir_h;
                if (scale < 1) scale = 1;
                int view_w = (int)ir_w * scale;
                int ox = (g_fb_w - view_w) / 2;
                blitScaled(framebuf, stride_words, ir_rgba, ir_w, ir_h, scale, ox);
            }

            // UI overlay (conditionally drawn)
            if (showUI) {
                drawRect(framebuf, stride_words, 0, 0, g_fb_w, 56, RGBA8_MAXALPHA(0,0,0));

                const char* msg1 = "L/R: resolution   ZL/ZR: exposure   Up/Down: colour";
                drawString(framebuf, stride_words, 10, 8, msg1, RGBA8_MAXALPHA(255,255,255), 2, 2);

                const char* msg2 = "Minus: IR LED   Y: auto-gain   X: log   LStick: hide UI";
                drawString(framebuf, stride_words, 10, 30, msg2, RGBA8_MAXALPHA(255,255,255), 2, 2);

                // Transient message: rejected exposure, watchdog recovery.
                if (ir_status_msg[0] && svcGetSystemTick() < ir_status_until) {
                    int w = (int)strlen(ir_status_msg) * (FONT_W*2 + 2);
                    drawRect(framebuf, stride_words, 4, 56, w+12, FONT_H*2+12,
                             RGBA8_MAXALPHA(0,0,0));
                    drawString(framebuf, stride_words, 10, 62, ir_status_msg,
                               RGBA8_MAXALPHA(255,170,170), 2, 2);
                }

                // Bottom-right: resolution + live FPS
                const char* res_names[] = {"20x15","40x30","80x60","160x120","320x240"};
                char status[80];
                snprintf(status, sizeof(status), "%s  %.1f FPS", res_names[current_format_index], fps_value);
                int status_width = (int)strlen(status) * (FONT_W*2 + 2);
                int status_height = FONT_H * 2;
                int status_x = g_fb_w - 10 - status_width;
                int status_y = g_fb_h - 10 - status_height;
                drawRect(framebuf, stride_words, status_x-6, status_y-6,
                         status_width+12, status_height+12, RGBA8_MAXALPHA(0,0,0));
                drawString(framebuf, stride_words, status_x, status_y,
                           status, RGBA8_MAXALPHA(180,255,180), 2, 2);

                // Bottom-left: colour mode, LED, exposure preset, gain
                const char* mode_label = useThermal ? "Thermal" : "Grayscale";
                char left_label[128];
                snprintf(left_label, sizeof(left_label),
                         "%s  | LED: %s  | %s %lluus G%u  | AGC: %s %.1fx",
                         mode_label, ledsEnabled ? "On" : "Off",
                         presets[preset_index].name,
                         (unsigned long long)(presets[preset_index].exposure_ns / 1000),
                         (unsigned)presets[preset_index].gain,
                         autoGain ? "On" : "Off", (double)agc_gain);
                int left_width = (int)strlen(left_label) * (FONT_W*2 + 2);
                int left_height = FONT_H * 2;
                int left_x = 10;
                int left_y = g_fb_h - 10 - left_height;
                drawRect(framebuf, stride_words, left_x-6, left_y-6,
                         left_width+12, left_height+12, RGBA8_MAXALPHA(0,0,0));
                drawString(framebuf, stride_words, left_x, left_y,
                           left_label, RGBA8_MAXALPHA(200,200,255), 2, 2);

                // Live stats in the left pillarbox
                char stats[64];
                snprintf(stats, sizeof(stats), "Frame: %llu", (unsigned long long)sampling_number);
                drawString(framebuf, stride_words, 12, 300, stats, RGBA8_MAXALPHA(150,150,150), 2, 2);
                snprintf(stats, sizeof(stats), "Noise: %u", ambient_noise);
                drawString(framebuf, stride_words, 12, 320, stats, RGBA8_MAXALPHA(150,150,150), 2, 2);
                snprintf(stats, sizeof(stats), "Start: 0x%08X", (unsigned)last_start_rc);
                drawString(framebuf, stride_words, 12, 340, stats, RGBA8_MAXALPHA(150,150,150), 2, 2);
            }

            if (log_overlay) drawLogOverlay(framebuf, stride_words);

            framebufferEnd(&fb);
        }
    }

    logMsg("=== exit ===");
    if (log_file) { fclose(log_file); log_file = NULL; }

    framebufferClose(&fb);
    irsStopImageProcessor(irhandle);
    free(ir_buffer);
    return 0;
}
