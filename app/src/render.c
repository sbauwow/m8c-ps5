// Copyright 2021 Jonne Kokkonen
// Released under the MIT licence, https://opensource.org/licenses/MIT

#include "render.h"
#if defined(PS4) || defined(PS5)
#include "ps4_shims.h" // PS5 build: compat redirect to the ps5_ implementations
#endif

#include <SDL.h>
#include <stdio.h>

#include "SDL2_inprint.h"
#include "command.h"
#include "fx_cube.h"

#include "font1.h"
#include "font2.h"
#include "font3.h"
#include "font4.h"
#include "font5.h"
#include "inline_font.h"

SDL_Window *win;
SDL_Renderer *rend;
SDL_Texture *maintexture;
SDL_Color background_color = (SDL_Color){.r = 0x00, .g = 0x00, .b = 0x00, .a = 0x00};

static uint32_t ticks_fps;
static int fps;
static int font_mode = -1;
static int m8_hardware_model = 0;
static int screen_offset_y = 0;
static int text_offset_y = 0;
static int waveform_max_height = 24;

static int texture_width = 320;
static int texture_height = 240;

struct inline_font *fonts[5] = {&font_v1_small, &font_v1_large, &font_v2_small, &font_v2_large,
                                &font_v2_huge};

uint8_t fullscreen = 0;

static uint8_t dirty = 0;

// Initializes SDL and creates a renderer and required surfaces
int initialize_sdl(const int init_fullscreen, const int init_use_gpu) {

#ifdef PS4
  // PS4 (znullptr SDL2): no GL/video-driver renderer. Follow the proven
  // OpenOrbis SDL2 sample: plain window -> window surface -> software
  // renderer. SDL_INIT_EVERYTHING + SDL_WINDOW_OPENGL crashes here
  // (CE-34878-0 with no diagnostics). SDL_Init is split per subsystem with
  // stage markers: the first boot hung somewhere in here and the log tail
  // has to say which call it was.
  ps4_stage("sdl_init_video");
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "SDL_Init(video): %s\n", SDL_GetError());
    return -1;
  }
  ps4_stage("sdl_init_joystick");
  if (SDL_InitSubSystem(SDL_INIT_JOYSTICK) != 0) {
    SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "SDL_Init(joystick): %s\n", SDL_GetError());
    return -1;
  }
  // GameController + audio are NOT initialised here: the proven OpenOrbis
  // sample inits only VIDEO|JOYSTICK before the window+software renderer.
  // They come in later via SDL_InitSubSystem from gamecontrollers.c and the
  // audio backends.
  ps4_stage("create_window");
  SDL_DisplayMode dm;
  int win_w = texture_width * 2;
  int win_h = texture_height * 2;
  if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0) {
    // PS4 SDL2 was only ever proven at the native video-out resolution
    // (720p/1080p); render_set_logical_size scales m8c's 480x270 to it.
    win_w = dm.w;
    win_h = dm.h;
  }
  SDL_Log("window %dx%d (display %dx%d)", win_w, win_h, dm.w, dm.h);
  win = SDL_CreateWindow("m8c", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h, 0);
  if (win == NULL) {
    SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "CreateWindow: %s\n", SDL_GetError());
    return -1;
  }
  ps4_stage("window_surface");
  SDL_Surface *wsurf = SDL_GetWindowSurface(win);
  if (wsurf == NULL) {
    SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "GetWindowSurface: %s\n", SDL_GetError());
    return -1;
  }
  ps4_stage("sw_renderer");
  rend = SDL_CreateSoftwareRenderer(wsurf);
  ps4_stage("sw_renderer_returned");
  if (rend == NULL) {
    SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "CreateSoftwareRenderer: %s\n", SDL_GetError());
    return -1;
  }
  atexit(SDL_Quit);
  (void)init_use_gpu; // unused on PS4 (software renderer is the path)
#else
  if (SDL_Init(SDL_INIT_EVERYTHING) != 0) {
    SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "SDL_Init: %s\n", SDL_GetError());
    return -1;
  }

  // SDL documentation recommends this
  atexit(SDL_Quit);

  win = SDL_CreateWindow(
      "m8c", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, texture_width * 2, texture_height * 2,
      SDL_WINDOW_SHOWN | SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | init_fullscreen);

  rend =
      SDL_CreateRenderer(win, -1, init_use_gpu ? SDL_RENDERER_ACCELERATED : SDL_RENDERER_SOFTWARE);
#endif

  ps4_stage("logical_size");
  SDL_RenderSetLogicalSize(rend, texture_width, texture_height);
  ps4_stage("create_texture");
  maintexture = NULL;
  maintexture = SDL_CreateTexture(rend, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET,
                                  texture_width, texture_height);
  ps4_stage("set_render_target");
  SDL_SetRenderTarget(rend, maintexture);
  ps4_stage("set_draw_color");
  SDL_SetRenderDrawColor(rend, background_color.r, background_color.g, background_color.b,
                         background_color.a);
  ps4_stage("render_clear");
  SDL_RenderClear(rend);
  ps4_stage("font_mode");
  set_font_mode(0);
  ps4_stage("sdl_tail_done");
  SDL_LogSetAllPriority(SDL_LOG_PRIORITY_INFO);

  dirty = 1;

  return 1;
}

static void change_font(struct inline_font *font) {
  kill_inline_font();
  inrenderer(rend);
  prepare_inline_font(font);
}

static void check_and_adjust_window_and_texture_size(const unsigned int new_width,
                                                     const unsigned int new_height) {

  int h, w;

  texture_width = new_width;
  texture_height = new_height;

  // Query window size and resize if smaller than default
  SDL_GetWindowSize(win, &w, &h);
  if (w < texture_width * 2 || h < texture_height * 2) {
    SDL_SetWindowSize(win, texture_width * 2, texture_height * 2);
  }

  SDL_DestroyTexture(maintexture);

  SDL_RenderSetLogicalSize(rend, texture_width, texture_height);

  maintexture = SDL_CreateTexture(rend, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET,
                                  texture_width, texture_height);

  SDL_SetRenderTarget(rend, maintexture);
}

// Set M8 hardware model in use. 0 = MK1, 1 = MK2
void set_m8_model(const unsigned int model) {

  switch (model) {
  case 1:
    m8_hardware_model = 1;
    check_and_adjust_window_and_texture_size(480, 320);
    break;
  default:
    m8_hardware_model = 0;
    check_and_adjust_window_and_texture_size(320, 240);
    break;
  }
}

void set_font_mode(int mode) {
  if (mode < 0 || mode > 2) {
    // bad font mode
    return;
  }
  if (m8_hardware_model == 1) {
    mode += 2;
  }
  if (font_mode == mode)
    return;

  font_mode = mode;
  screen_offset_y = fonts[mode]->screen_offset_y;
  text_offset_y = fonts[mode]->text_offset_y;
  waveform_max_height = fonts[mode]->waveform_max_height;

  change_font(fonts[mode]);
  SDL_LogDebug(SDL_LOG_CATEGORY_RENDER, "Font mode %i, Screen offset %i", mode, screen_offset_y);
}

void close_renderer() {
  kill_inline_font();
  SDL_DestroyTexture(maintexture);
  SDL_DestroyRenderer(rend);
  SDL_DestroyWindow(win);
}

void toggle_fullscreen() {

  const int fullscreen_state = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN;

  SDL_SetWindowFullscreen(win, fullscreen_state ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
  SDL_ShowCursor(fullscreen_state);

  dirty = 1;
}

int draw_character(struct draw_character_command *command) {

  const uint32_t fgcolor =
      command->foreground.r << 16 | command->foreground.g << 8 | command->foreground.b;
  const uint32_t bgcolor =
      command->background.r << 16 | command->background.g << 8 | command->background.b;

  /* Notes:
     If large font is enabled, offset the screen elements by a fixed amount.
     If background and foreground colors are the same, draw transparent
     background. Due to the font bitmaps, a different pixel offset is needed for
     both*/

  inprint(rend, (char *)&command->c, command->pos.x,
          command->pos.y + text_offset_y + screen_offset_y, fgcolor,
          bgcolor);

  dirty = 1;

  return 1;
}

void draw_rectangle(struct draw_rectangle_command *command) {

  SDL_Rect render_rect;

  render_rect.x = command->pos.x;
  render_rect.y = command->pos.y + screen_offset_y;
  render_rect.h = command->size.height;
  render_rect.w = command->size.width;

  // Background color changed
  if (render_rect.x == 0 && render_rect.y <= 0 && render_rect.w == texture_width &&
      render_rect.h >= texture_height) {
    SDL_LogDebug(SDL_LOG_CATEGORY_SYSTEM, "BG color change: %d %d %d", command->color.r,
                 command->color.g, command->color.b);
    background_color.r = command->color.r;
    background_color.g = command->color.g;
    background_color.b = command->color.b;
    background_color.a = 0xFF;
    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "x:%i, y:%i, w:%i, h:%i", render_rect.x,
                 render_rect.y, render_rect.w, render_rect.h);

#ifdef __ANDROID__
    int bgcolor = (command->color.r << 16) | (command->color.g << 8) | command->color.b;
    SDL_AndroidSendMessage(0x8001, bgcolor);
#endif
  }

  SDL_SetRenderDrawColor(rend, command->color.r, command->color.g, command->color.b, 0xFF);
  SDL_RenderFillRect(rend, &render_rect);

  dirty = 1;
}

void draw_waveform(struct draw_oscilloscope_waveform_command *command) {

  static uint8_t wfm_cleared = 0;
  static int prev_waveform_size = 0;

  // If the waveform is not being displayed and it's already been cleared, skip
  // rendering it
  if (!(wfm_cleared && command->waveform_size == 0)) {

    SDL_Rect wf_rect;
    if (command->waveform_size > 0) {
      wf_rect.x = texture_width - command->waveform_size;
      wf_rect.y = 0;
      wf_rect.w = command->waveform_size;
      wf_rect.h = waveform_max_height + 1;
    } else {
      wf_rect.x = texture_width - prev_waveform_size;
      wf_rect.y = 0;
      wf_rect.w = prev_waveform_size;
      wf_rect.h = waveform_max_height + 1;
    }
    prev_waveform_size = command->waveform_size;

    SDL_SetRenderDrawColor(rend, background_color.r, background_color.g, background_color.b,
                           background_color.a);
    SDL_RenderFillRect(rend, &wf_rect);

    SDL_SetRenderDrawColor(rend, command->color.r, command->color.g, command->color.b, 255);

    // Create a SDL_Point array of the waveform pixels for batch drawing
    SDL_Point waveform_points[command->waveform_size];

    for (int i = 0; i < command->waveform_size; i++) {
      // Limit value because the oscilloscope commands seem to glitch
      // occasionally
      if (command->waveform[i] > waveform_max_height) {
        command->waveform[i] = waveform_max_height;
      }
      waveform_points[i].x = i + wf_rect.x;
      waveform_points[i].y = command->waveform[i];
    }

    SDL_RenderDrawPoints(rend, waveform_points, command->waveform_size);

    // The packet we just drew was an empty waveform
    if (command->waveform_size == 0) {
      wfm_cleared = 1;
    } else {
      wfm_cleared = 0;
    }

    dirty = 1;
  }
}

void display_keyjazz_overlay(const uint8_t show, const uint8_t base_octave,
                             const uint8_t velocity) {

  const Uint16 overlay_offset_x = texture_width - (fonts[font_mode]->glyph_x * 7 + 1);
  const Uint16 overlay_offset_y = texture_height - (fonts[font_mode]->glyph_y + 1);
  const Uint32 bgcolor =
      background_color.r << 16 | background_color.g << 8 | background_color.b;

  if (show) {
    char overlay_text[7];
    snprintf(overlay_text, sizeof(overlay_text), "%02X %u", velocity, base_octave);
    inprint(rend, overlay_text, overlay_offset_x, overlay_offset_y, 0xC8C8C8, bgcolor);
    inprint(rend, "*", overlay_offset_x + (fonts[font_mode]->glyph_x * 5 + 5), overlay_offset_y,
            0xFF0000, bgcolor);
  } else {
    inprint(rend, "      ", overlay_offset_x, overlay_offset_y, 0xC8C8C8, bgcolor);
  }

  dirty = 1;
}

void render_screen() {
  if (dirty) {
    dirty = 0;
    // NOTE(PS4): deliberately NO instrumentation in this hot path. The
    // input-works build had rs: markers; today's flicker-crash builds had
    // them plus a read counter — all stripped to return to the proven
    // minimal render path.
    SDL_SetRenderTarget(rend, NULL);

    SDL_SetRenderDrawColor(rend, background_color.r, background_color.g, background_color.b,
                           background_color.a);

    SDL_RenderClear(rend);
    SDL_RenderCopy(rend, maintexture, NULL, NULL);
    SDL_RenderPresent(rend);
    // znullptr PS4 SDL2: the window surface IS the framebuffer and
    // SDL_UpdateWindowSurface performs the actual flip (the OpenOrbis sample
    // presents this way; SDL_RenderPresent alone shows nothing).
    SDL_UpdateWindowSurface(win);
    SDL_SetRenderTarget(rend, maintexture);

    fps++;

    if (SDL_GetTicks() - ticks_fps > 5000) {
      ticks_fps = SDL_GetTicks();
      SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "%.1f fps\n", (float)fps / 5);
      fps = 0;
    }
  }
}

void screensaver_init() {
  set_font_mode(1);
  fx_cube_init(rend, (SDL_Color){255, 255, 255, 255}, texture_width, texture_height,
               fonts[font_mode]->glyph_x);
  SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Screensaver initialized");
}

void screensaver_draw() {
  fx_cube_update();
  dirty = 1;
}

void screensaver_destroy() {
  fx_cube_destroy();
  set_font_mode(0);
  SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Screensaver destroyed");
}
