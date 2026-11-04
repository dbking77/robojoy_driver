#include "robojoy_session.h"

#include <SDL2/SDL.h>

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef ROBOJOY_HAVE_SDL_TTF
#include <SDL2/SDL_ttf.h>
#else
typedef struct TTF_Font TTF_Font;
TTF_Font *TTF_OpenFont(const char *file, int ptsize);
SDL_Surface *TTF_RenderUTF8_Blended(TTF_Font *font, const char *text, SDL_Color fg);
int TTF_SizeUTF8(TTF_Font *font, const char *text, int *w, int *h);
void TTF_CloseFont(TTF_Font *font);
int TTF_Init(void);
void TTF_Quit(void);
#endif

#define WIN_W 980
#define WIN_H 760
#define PAD_R 130

#define FLAG_ENABLE 0x01
#define FLAG_RIGHT_BUTTON 0x02
#define FLAG_LEFT_BUTTON 0x04

static const char *FONT_CANDIDATES[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
};

static volatile sig_atomic_t g_stop;

static const SDL_Color COL_BG = { 30, 32, 36, 255 };
static const SDL_Color COL_TEXT = { 230, 230, 230, 255 };
static const SDL_Color COL_MUTED = { 160, 164, 172, 255 };
static const SDL_Color COL_PAD = { 58, 62, 72, 255 };
static const SDL_Color COL_PAD_HOT = { 64, 86, 118, 255 };
static const SDL_Color COL_KNOB = { 70, 130, 230, 255 };
static const SDL_Color COL_BTN = { 70, 74, 84, 255 };
static const SDL_Color COL_BTN_HOT = { 110, 120, 140, 255 };
static const SDL_Color COL_OFF = { 150, 52, 52, 255 };
static const SDL_Color COL_ON = { 36, 140, 72, 255 };

struct stick_pad
{
    int cx;
    int cy;
    int radius;
    int fwd;
    int right;
    int knob_dx;
    int knob_dy;
    int active;
    const char *name;
};

static void on_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}

static uint64_t monotonic_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static void set_color(SDL_Renderer *renderer, SDL_Color color)
{
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
}

static void fill_circle(SDL_Renderer *renderer, int cx, int cy, int radius)
{
    int y;
    for (y = -radius; y <= radius; y++)
    {
        int span = 0;
        int remain = radius * radius - y * y;
        while ((span + 1) * (span + 1) <= remain)
            span++;
        SDL_RenderDrawLine(renderer, cx - span, cy + y, cx + span, cy + y);
    }
}

static int in_rect(SDL_Rect rect, int x, int y)
{
    return x >= rect.x && y >= rect.y && x < rect.x + rect.w && y < rect.y + rect.h;
}

static int in_pad(const struct stick_pad *pad, int x, int y)
{
    int dx = x - pad->cx;
    int dy = y - pad->cy;
    return dx * dx + dy * dy <= pad->radius * pad->radius;
}

/* Same mapping as the LVGL sim pad: up is forward, right is positive. */
static void move_pad(struct stick_pad *pad, int x, int y)
{
    int dx = x - pad->cx;
    int dy = y - pad->cy;
    int radius = pad->radius;

    if (dx > radius)
        dx = radius;
    else if (dx < -radius)
        dx = -radius;
    if (dy > radius)
        dy = radius;
    else if (dy < -radius)
        dy = -radius;

    pad->knob_dx = dx;
    pad->knob_dy = dy;
    pad->right = dx * 100 / radius;
    pad->fwd = -dy * 100 / radius;
}

static void center_pad(struct stick_pad *pad)
{
    pad->active = 0;
    pad->fwd = 0;
    pad->right = 0;
    pad->knob_dx = 0;
    pad->knob_dy = 0;
}

static void draw_text(SDL_Renderer *renderer, TTF_Font *font, int x, int y, SDL_Color color, const char *text)
{
    SDL_Surface *surface;
    SDL_Texture *texture;
    SDL_Rect dest;

    if (!text || text[0] == '\0')
        return;
    surface = TTF_RenderUTF8_Blended(font, text, color);
    if (!surface)
        return;
    texture = SDL_CreateTextureFromSurface(renderer, surface);
    dest.x = x;
    dest.y = y;
    dest.w = surface->w;
    dest.h = surface->h;
    SDL_FreeSurface(surface);
    if (!texture)
        return;
    SDL_RenderCopy(renderer, texture, NULL, &dest);
    SDL_DestroyTexture(texture);
}

static void draw_text_centered(SDL_Renderer *renderer, TTF_Font *font, SDL_Rect box, SDL_Color color, const char *text)
{
    int width = 0;
    int height = 0;

    if (TTF_SizeUTF8(font, text, &width, &height) != 0)
        return;
    draw_text(renderer, font, box.x + (box.w - width) / 2, box.y + (box.h - height) / 2, color, text);
}

static void draw_pad(SDL_Renderer *renderer, TTF_Font *font, const struct stick_pad *pad)
{
    char line[64];

    set_color(renderer, pad->active ? COL_PAD_HOT : COL_PAD);
    fill_circle(renderer, pad->cx, pad->cy, pad->radius);
    set_color(renderer, COL_MUTED);
    SDL_RenderDrawLine(renderer, pad->cx - pad->radius + 16, pad->cy, pad->cx + pad->radius - 16, pad->cy);
    SDL_RenderDrawLine(renderer, pad->cx, pad->cy - pad->radius + 16, pad->cx, pad->cy + pad->radius - 16);
    set_color(renderer, COL_KNOB);
    fill_circle(renderer, pad->cx + pad->knob_dx, pad->cy + pad->knob_dy, 22);
    snprintf(line, sizeof line, "%s   fwd %d   right %d", pad->name, pad->fwd, pad->right);
    draw_text(renderer, font, pad->cx - pad->radius, pad->cy + pad->radius + 8, COL_TEXT, line);
}

static void draw_button(SDL_Renderer *renderer, TTF_Font *font, SDL_Rect rect, SDL_Color fill, const char *label)
{
    set_color(renderer, fill);
    SDL_RenderFillRect(renderer, &rect);
    draw_text_centered(renderer, font, rect, COL_TEXT, label);
}

static void draw_checkbox(SDL_Renderer *renderer, TTF_Font *font, SDL_Rect box, int checked, const char *label)
{
    set_color(renderer, checked ? COL_ON : COL_BTN);
    SDL_RenderFillRect(renderer, &box);
    set_color(renderer, COL_TEXT);
    SDL_RenderDrawRect(renderer, &box);
    draw_text(renderer, font, box.x + box.w + 8, box.y - 2, COL_TEXT, label);
}

static int slider_value(SDL_Rect track, int x)
{
    if (x <= track.x)
        return 0;
    if (x >= track.x + track.w)
        return 100;
    return (x - track.x) * 100 / track.w;
}

static void draw_slider(SDL_Renderer *renderer, TTF_Font *font, SDL_Rect track, int value, const char *label)
{
    char line[48];
    int knob_x = track.x + track.w * value / 100;
    SDL_Rect hit = track;

    hit.y -= 6;
    hit.h += 12;
    set_color(renderer, COL_PAD);
    SDL_RenderFillRect(renderer, &hit);
    set_color(renderer, COL_KNOB);
    fill_circle(renderer, knob_x, track.y + track.h / 2, 9);
    snprintf(line, sizeof line, "%s %d", label, value);
    draw_text(renderer, font, track.x, track.y - 28, COL_TEXT, line);
}

static void format_rsp_flags(uint8_t flags, char *buf, size_t len)
{
    static const struct
    {
        uint8_t bit;
        const char *name;
    } table[] = {
        { 0x01, "enabled" },
        { 0x02, "fault" },
        { 0x04, "low_batt" },
        { 0x08, "timeout" },
        { 0x10, "bad_key" },
    };
    size_t used = 0;
    size_t i;

    if (len == 0)
        return;
    buf[0] = '\0';
    for (i = 0; i < sizeof table / sizeof table[0]; i++)
    {
        int wrote;
        if ((flags & table[i].bit) == 0)
            continue;
        wrote = snprintf(buf + used, len - used, "%s%s", used ? "," : "", table[i].name);
        if (wrote < 0 || (size_t)wrote >= len - used)
            return;
        used += (size_t)wrote;
    }
    if (used == 0)
        snprintf(buf, len, "none");
}

static void print_usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "\n"
            "SDL window with two mouse sticks. Drag inside a circle; release returns it to center.\n"
            "Drive enable stays off until the Enable button is clicked.\n"
            "\n"
            "  -n, --name NAME     Advertising name (default ROBODRIVE_2025)\n"
            "  -a, --address ADDR  Bluetooth address AA:BB:CC:DD:EE:FF\n"
            "  -A, --adapter HCI   Adapter name (default hci0)\n"
            "  -h, --help\n"
            "\n"
            "Known names: BOO_FRIENDS, ROBODRIVE_2025, AUSTIN_OCT2026, BOX_BOX_BOX\n",
            argv0);
}

static TTF_Font *open_font(int size)
{
    size_t i;
    for (i = 0; i < sizeof FONT_CANDIDATES / sizeof FONT_CANDIDATES[0]; i++)
    {
        TTF_Font *font = TTF_OpenFont(FONT_CANDIDATES[i], size);
        if (font)
            return font;
    }
    return NULL;
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        { "name", required_argument, NULL, 'n' },
        { "address", required_argument, NULL, 'a' },
        { "adapter", required_argument, NULL, 'A' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };

    const char *name = NULL;
    const char *address = NULL;
    const char *adapter = NULL;
    robojoy_session_config cfg;
    robojoy_session *session = NULL;
    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    TTF_Font *font = NULL;
    TTF_Font *font_small = NULL;
    struct stick_pad left = { 210, 250, PAD_R, 0, 0, 0, 0, 0, "Left" };
    struct stick_pad right = { 520, 250, PAD_R, 0, 0, 0, 0, 0, "Right" };
    SDL_Rect left_button = { 160, 460, 100, 44 };
    SDL_Rect right_button = { 470, 460, 100, 44 };
    SDL_Rect enable_button = { 720, 430, 220, 64 };
    SDL_Rect rev1_box = { 36, 568, 22, 22 };
    SDL_Rect rev2_box = { 320, 568, 22, 22 };
    SDL_Rect swap_box = { 620, 568, 22, 22 };
    SDL_Rect full_track = { 36, 660, 280, 16 };
    SDL_Rect turn_track = { 400, 660, 280, 16 };
    SDL_Rect read_button = { 720, 620, 220, 36 };
    SDL_Rect set_button = { 720, 664, 104, 36 };
    SDL_Rect save_button = { 836, 664, 104, 36 };
    struct robodrive_diff_drive_config drive_config = {
        .flags = 0,
        .full_scale = 100,
        .turn_scale = 100,
    };
    int left_held = 0;
    int right_held = 0;
    int drive_enabled = 0;
    int link_started = 0;
    int link_was_ready = 0;
    int drag_full = 0;
    int drag_turn = 0;
    char config_status[96] = "config not read";
    int opt;
    uint32_t shown_rx = 0;
    uint64_t next_rx_sample = 0;
    int sdl_ready = 0;
    int ttf_ready = 0;
    int exit_code = 1;

    while ((opt = getopt_long(argc, argv, "n:a:A:h", options, NULL)) != -1)
    {
        switch (opt)
        {
        case 'n':
            name = optarg;
            break;
        case 'a':
            address = optarg;
            break;
        case 'A':
            adapter = optarg;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }
    if (optind != argc)
    {
        print_usage(argv[0]);
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    memset(&cfg, 0, sizeof cfg);
    cfg.adapter = adapter;
    cfg.device_name = name ? name : "ROBODRIVE_2025";
    cfg.address = address;
    session = robojoy_session_create(&cfg);
    if (!session)
    {
        fprintf(stderr, "robojoy: out of memory\n");
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        goto done;
    }
    sdl_ready = 1;
    if (TTF_Init() != 0)
    {
        fprintf(stderr, "TTF_Init: %s\n", SDL_GetError());
        goto done;
    }
    ttf_ready = 1;
    font = open_font(22);
    font_small = open_font(18);
    if (!font || !font_small)
    {
        fprintf(stderr, "robojoy: failed to open a sans font\n");
        goto done;
    }

    window = SDL_CreateWindow("RoboJoy", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              WIN_W, WIN_H, SDL_WINDOW_SHOWN);
    renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : NULL;
    if (!renderer)
    {
        fprintf(stderr, "SDL renderer: %s\n", SDL_GetError());
        goto done;
    }

    fprintf(stderr, "robojoy: gui target %s, drive off until Enable is clicked\n",
            robojoy_session_target(session));

    while (!g_stop)
    {
        SDL_Event event;
        struct robodrive_joy_cmd cmd;
        char line[160];
        char flags[64];
        const struct robodrive_joy_resp *rsp;
        int rssi;
        uint64_t now;
        int config_action = 0;

        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_QUIT)
                g_stop = 1;
            else if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)
                g_stop = 1;
            else if (event.type == SDL_WINDOWEVENT &&
                     event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
            {
                center_pad(&left);
                center_pad(&right);
                left_held = 0;
                right_held = 0;
                SDL_CaptureMouse(SDL_FALSE);
            }
            else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT)
            {
                int x = event.button.x;
                int y = event.button.y;
                if (in_rect(enable_button, x, y))
                    drive_enabled = !drive_enabled;
                else if (in_rect(read_button, x, y))
                    config_action = 1;
                else if (in_rect(set_button, x, y))
                    config_action = 2;
                else if (in_rect(save_button, x, y))
                    config_action = 3;
                else if (in_rect(rev1_box, x, y) || in_rect((SDL_Rect){ rev1_box.x, rev1_box.y, 200, rev1_box.h }, x, y))
                    drive_config.flags ^= ROBODRIVE_DIFF_DRIVE_FLAG_REVERSE_MOTOR_1;
                else if (in_rect(rev2_box, x, y) || in_rect((SDL_Rect){ rev2_box.x, rev2_box.y, 220, rev2_box.h }, x, y))
                    drive_config.flags ^= ROBODRIVE_DIFF_DRIVE_FLAG_REVERSE_MOTOR_2;
                else if (in_rect(swap_box, x, y) || in_rect((SDL_Rect){ swap_box.x, swap_box.y, 160, swap_box.h }, x, y))
                    drive_config.flags ^= ROBODRIVE_DIFF_DRIVE_FLAG_SWAP_MOTORS;
                else if (in_rect((SDL_Rect){ full_track.x, full_track.y - 8, full_track.w, full_track.h + 16 }, x, y))
                {
                    drag_full = 1;
                    drive_config.full_scale = (uint8_t)slider_value(full_track, x);
                }
                else if (in_rect((SDL_Rect){ turn_track.x, turn_track.y - 8, turn_track.w, turn_track.h + 16 }, x, y))
                {
                    drag_turn = 1;
                    drive_config.turn_scale = (uint8_t)slider_value(turn_track, x);
                }
                else if (in_rect(left_button, x, y))
                {
                    left_held = 1;
                    SDL_CaptureMouse(SDL_TRUE);
                }
                else if (in_rect(right_button, x, y))
                {
                    right_held = 1;
                    SDL_CaptureMouse(SDL_TRUE);
                }
                else if (in_pad(&left, x, y))
                {
                    left.active = 1;
                    move_pad(&left, x, y);
                    SDL_CaptureMouse(SDL_TRUE);
                }
                else if (in_pad(&right, x, y))
                {
                    right.active = 1;
                    move_pad(&right, x, y);
                    SDL_CaptureMouse(SDL_TRUE);
                }
            }
            else if (event.type == SDL_MOUSEMOTION)
            {
                if (drag_full)
                    drive_config.full_scale = (uint8_t)slider_value(full_track, event.motion.x);
                else if (drag_turn)
                    drive_config.turn_scale = (uint8_t)slider_value(turn_track, event.motion.x);
                if (left.active)
                    move_pad(&left, event.motion.x, event.motion.y);
                if (right.active)
                    move_pad(&right, event.motion.x, event.motion.y);
            }
            else if (event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT)
            {
                drag_full = 0;
                drag_turn = 0;
                center_pad(&left);
                center_pad(&right);
                left_held = 0;
                right_held = 0;
                SDL_CaptureMouse(SDL_FALSE);
            }
        }

        if (g_stop)
            break;

        memset(&cmd, 0, sizeof cmd);
        cmd.l_joy_fwd = (int8_t)left.fwd;
        cmd.l_joy_right = (int8_t)left.right;
        cmd.r_joy_fwd = (int8_t)right.fwd;
        cmd.r_joy_right = (int8_t)right.right;
        if (drive_enabled)
            cmd.flags |= FLAG_ENABLE;
        if (right_held)
            cmd.flags |= FLAG_RIGHT_BUTTON;
        if (left_held)
            cmd.flags |= FLAG_LEFT_BUTTON;

        if (!link_started)
        {
            const char *error;
            int rc = robojoy_session_connect(session, 15);
            error = robojoy_session_last_error(session);
            if (rc == 0 || (error && strstr(error, "timed out waiting") != NULL))
                link_started = 1;
        }
        else
        {
            int ready = robojoy_session_ready(session);
            if (ready && !link_was_ready && config_action == 0)
                config_action = 1;
            link_was_ready = ready;

            if (config_action != 0)
            {
                const char *error;
                if (!ready)
                    snprintf(config_status, sizeof config_status, "link down");
                else if (config_action == 1)
                {
                    struct robodrive_diff_drive_config got;
                    if (robojoy_session_config_read(session, &got, 2000) == 0)
                    {
                        drive_config = got;
                        snprintf(config_status, sizeof config_status, "read ok");
                    }
                    else
                    {
                        error = robojoy_session_last_error(session);
                        snprintf(config_status, sizeof config_status, "%s", error[0] ? error : "read failed");
                    }
                }
                else if (config_action == 2)
                {
                    if (robojoy_session_config_set(session, &drive_config, 2000) == 0)
                        snprintf(config_status, sizeof config_status, "set ok");
                    else
                    {
                        error = robojoy_session_last_error(session);
                        snprintf(config_status, sizeof config_status, "%s", error[0] ? error : "set failed");
                    }
                }
                else if (robojoy_session_config_save(session, &drive_config, 2000) == 0)
                    snprintf(config_status, sizeof config_status, "saved");
                else
                {
                    error = robojoy_session_last_error(session);
                    snprintf(config_status, sizeof config_status, "%s", error[0] ? error : "save failed");
                }
            }

            robojoy_session_step(session, &cmd, 0);
        }

        now = monotonic_us();
        if (now >= next_rx_sample)
        {
            shown_rx = robojoy_session_take_response_count(session);
            next_rx_sample = now + 1000000ull;
        }

        set_color(renderer, COL_BG);
        SDL_RenderClear(renderer);

        snprintf(line, sizeof line, "RoboJoy    %s", robojoy_session_target(session));
        draw_text(renderer, font, 36, 24, COL_TEXT, line);
        draw_text(renderer, font_small, 36, 56, COL_MUTED,
                  "Drag inside a circle. Release returns the stick to center.");

        draw_pad(renderer, font_small, &left);
        draw_pad(renderer, font_small, &right);
        draw_button(renderer, font_small, left_button, left_held ? COL_BTN_HOT : COL_BTN, "L click");
        draw_button(renderer, font_small, right_button, right_held ? COL_BTN_HOT : COL_BTN, "R click");
        draw_button(renderer, font, enable_button, drive_enabled ? COL_ON : COL_OFF,
                    drive_enabled ? "Drive enabled" : "Enable drive");

        rsp = robojoy_session_response(session);
        rssi = robojoy_session_rssi(session);
        if (!robojoy_session_ready(session))
        {
            const char *error = robojoy_session_last_error(session);
            snprintf(line, sizeof line, "Link down");
            draw_text(renderer, font, 720, 80, COL_TEXT, line);
            if (error && error[0] && strstr(error, "timed out waiting") == NULL)
                draw_text(renderer, font_small, 720, 116, COL_MUTED, error);
            else
                draw_text(renderer, font_small, 720, 116, COL_MUTED, "scanning");
        }
        else if (!rsp)
        {
            draw_text(renderer, font, 720, 80, COL_TEXT, "Link up");
            draw_text(renderer, font_small, 720, 116, COL_MUTED, "waiting for feedback");
        }
        else
        {
            format_rsp_flags(rsp->flags, flags, sizeof flags);
            draw_text(renderer, font, 720, 80, COL_TEXT, "Link up");
            snprintf(line, sizeof line, "fw %u.%u",
                     (unsigned)get_fw_version_major(rsp->hdr.fw_version),
                     (unsigned)get_fw_version_minor(rsp->hdr.fw_version));
            draw_text(renderer, font_small, 720, 116, COL_TEXT, line);
            snprintf(line, sizeof line, "seq %u   key %u", (unsigned)rsp->hdr.seqnum, (unsigned)rsp->key);
            draw_text(renderer, font_small, 720, 142, COL_TEXT, line);
            snprintf(line, sizeof line, "flags 0x%02x %s", (unsigned)rsp->flags, flags);
            draw_text(renderer, font_small, 720, 168, COL_TEXT, line);
            snprintf(line, sizeof line, "batt %u%%", (unsigned)rsp->batt_percent);
            draw_text(renderer, font_small, 720, 194, COL_TEXT, line);
            snprintf(line, sizeof line, "cpu %us   motor %us",
                     (unsigned)rsp->cpu_uptime, (unsigned)rsp->motor_uptime);
            draw_text(renderer, font_small, 720, 220, COL_TEXT, line);
            snprintf(line, sizeof line, "rtt %u ms   rx %u/s",
                     (unsigned)robojoy_session_latency_ms(session), (unsigned)shown_rx);
            draw_text(renderer, font_small, 720, 246, COL_TEXT, line);
            if (rssi != 0)
                snprintf(line, sizeof line, "rssi %d dBm", rssi);
            else
                snprintf(line, sizeof line, "rssi n/a");
            draw_text(renderer, font_small, 720, 272, COL_TEXT, line);
        }

        draw_text(renderer, font, 36, 528, COL_TEXT, "Diff drive");
        draw_checkbox(renderer, font_small, rev1_box,
                      drive_config.flags & ROBODRIVE_DIFF_DRIVE_FLAG_REVERSE_MOTOR_1, "Reverse motor 1");
        draw_checkbox(renderer, font_small, rev2_box,
                      drive_config.flags & ROBODRIVE_DIFF_DRIVE_FLAG_REVERSE_MOTOR_2, "Reverse motor 2");
        draw_checkbox(renderer, font_small, swap_box,
                      drive_config.flags & ROBODRIVE_DIFF_DRIVE_FLAG_SWAP_MOTORS, "Swap motors");
        draw_slider(renderer, font_small, full_track, drive_config.full_scale, "Full scale");
        draw_slider(renderer, font_small, turn_track, drive_config.turn_scale, "Turn scale");
        draw_button(renderer, font_small, read_button, COL_BTN, "Read");
        draw_button(renderer, font_small, set_button, COL_BTN, "Set");
        draw_button(renderer, font_small, save_button, COL_BTN, "Save");
        draw_text(renderer, font_small, 36, 700, COL_MUTED, config_status);

        SDL_RenderPresent(renderer);
    }

    exit_code = 0;

done:
    if (session)
        robojoy_session_destroy(session);
    if (font)
        TTF_CloseFont(font);
    if (font_small)
        TTF_CloseFont(font_small);
    if (renderer)
        SDL_DestroyRenderer(renderer);
    if (window)
        SDL_DestroyWindow(window);
    if (ttf_ready)
        TTF_Quit();
    if (sdl_ready)
        SDL_Quit();
    return exit_code;
}
