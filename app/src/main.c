// Copyright 2021 Jonne Kokkonen
// Released under the MIT licence, https://opensource.org/licenses/MIT

/* Uncomment this line to enable debug messages or call make with `make
   CFLAGS=-DDEBUG_MSG` */
// #define DEBUG_MSG

#include <SDL.h>
#include <signal.h>

#include "SDL2_inprint.h"
#include "audio.h"
#include "command.h"
#include "config.h"
#include "input.h"
#include "gamecontrollers.h"
#include "render.h"
#include "serial.h"
#include "slip.h"
#if defined(PS4) || defined(PS5)
#include "ps4_shims.h"
#include "usb_ps4.h"
#endif
#ifdef PS5
#include "ps4_shims.h" // PS5 build: compat redirect to the ps5_ implementations
#include "usb_ps4.h"   // PS5 build: compat header over ps5_usbd + usb_ps5.c
#endif

enum state { QUIT, WAIT_FOR_DEVICE, RUN };

enum state run = WAIT_FOR_DEVICE;
uint8_t need_display_reset = 0;

// Handles CTRL+C / SIGINT
void intHandler() { run = QUIT; }

void close_serial_port() { disconnect(); }

int main(const int argc, char *argv[]) {

  if (argc == 2 && SDL_strcmp(argv[1], "--list") == 0) {
    return list_devices();
  }

  char *preferred_device = NULL;
  if (argc == 3 && SDL_strcmp(argv[1], "--dev") == 0) {
    preferred_device = argv[2];
    SDL_Log("Using preferred device %s.\n", preferred_device);
  }

  char *config_filename = NULL;
  if (argc == 3 && SDL_strcmp(argv[1], "--config") == 0) {
    config_filename = argv[2];
    SDL_Log("Using config file %s.\n", config_filename);
  }

  // Initialize the config to defaults read in the params from the
  // configfile if present
  config_params_s conf = init_config(config_filename);
  read_config(&conf);
#if defined(PS4) || defined(PS5)
  ps4_usb_set_audio_wanted(conf.audio_enabled == 1);
#endif

  // allocate memory for serial buffer
  uint8_t *serial_buf = SDL_malloc(serial_read_size);

  static uint8_t slip_buffer[serial_read_size]; // SLIP command buffer

  SDL_zero(slip_buffer);

  // settings for the slip packet handler
  static const slip_descriptor_s slip_descriptor = {
      .buf = slip_buffer,
      .buf_size = sizeof(slip_buffer),
      .recv_message = process_command, // the function where complete slip
                                       // packets are processed further
  };

  static slip_handler_s slip;

  uint8_t prev_input = 0;
  uint8_t prev_note = 0;
  uint16_t zerobyte_packets = 0; // used to detect device disconnection

  signal(SIGINT, intHandler);
  signal(SIGTERM, intHandler);
#ifdef SIGQUIT
  signal(SIGQUIT, intHandler);
#endif
#if defined(PS4) || defined(PS5)
  ps4_log_init(); // SDL_Log -> /data/m8c.log before anything else logs
  SDL_Log("m8c PS4 starting");
  ps4_stage("BUILD-C7"); // build identity marker
#endif
#ifdef PS5
  const int killed_pid = ps5_kill_previous_instance();
  ps4_log_init(); // SDL_Log -> /data/m8c.log before anything else logs
  if (killed_pid) {
    SDL_Log("killed previous m8c instance, pid %d", killed_pid);
  }
  SDL_Log("m8c PS5 starting");
  ps4_stage("BUILD-C7-PS5"); // build identity marker
#endif
  slip_init(&slip, &slip_descriptor);

  // First device detection to avoid SDL init if it isn't necessary. To be run
  // only if we shouldn't wait for M8 to be connected.
  if (conf.wait_for_device == 0) {
    if (init_serial(1, preferred_device) == 0) {
      SDL_free(serial_buf);
      return -1;
    }
  }

  // initialize all SDL systems
  ps4_stage("initialize_sdl");
  if (initialize_sdl(conf.init_fullscreen, conf.init_use_gpu) == -1)
    run = QUIT;
  ps4_stage("sdl_ready");

  // initial scan for (existing) game controllers
  // PS4: SDL_INIT_GAMECONTROLLER must be live before opening controllers.
  // (Removed during the boot-hang hunt; the stage logs later proved this
  // subsystem initialises fine on 6.72 - the hang was the software renderer.)
  ps4_stage("gc_subsystem");
  if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
    SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "SDL_Init(gamecontroller): %s\n", SDL_GetError());
  }
  gamecontrollers_initialize();
  ps4_stage("controllers_done");

#ifdef DEBUG_MSG
  SDL_LogSetAllPriority(SDL_LOG_PRIORITY_DEBUG);
#endif

  // main loop begin
#if defined(PS4) || defined(PS5)
  ps4_stage("main_loop");
#endif
  do {
    // try to init serial port
    int port_inited = init_serial(1, preferred_device);
#if defined(PS4) || defined(PS5)
    ps4_stage(port_inited == 1 ? "serial_ok" : "serial_absent");
#endif
    // if port init was successful, try to enable and reset display
    if (port_inited == 1 && enable_and_reset_display() == 1) {
#if defined(PS4) || defined(PS5)
      ps4_stage("display_enabled");
#endif
      // if audio routing is enabled, try to initialize audio devices
      if (conf.audio_enabled == 1) {
#if defined(PS4) || defined(PS5)
        ps4_stage("audio_init_enter");
#endif
        audio_init(conf.audio_buffer_size, conf.audio_device_name);
#if defined(PS4) || defined(PS5)
        ps4_stage("audio_init_return");
#endif
        // if audio is enabled, reset the display for second time to avoid glitches
        reset_display();
      }
      run = RUN;
    } else {
      SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "Device not detected on begin loop.");
      if (conf.wait_for_device == 1) {
        run = WAIT_FOR_DEVICE;
      } else {
        run = QUIT;
      }
    }

    // wait until device is connected
    if (conf.wait_for_device == 1) {
      static uint32_t ticks_poll_device = 0;
      static uint32_t ticks_update_screen = 0;

      if (port_inited == 0) {
        screensaver_init();
      }

      while (run == WAIT_FOR_DEVICE) {
        // get current input
        const input_msg_s input = get_input_msg(&conf);
        if (input.type == special && input.value == msg_quit) {
          SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "Input message QUIT.");
          run = QUIT;
        }

        if (SDL_GetTicks() - ticks_update_screen > 16) {
          ticks_update_screen = SDL_GetTicks();
          screensaver_draw();
          render_screen();
        }

        // Poll for M8 device every second
        if (port_inited == 0 && SDL_GetTicks() - ticks_poll_device > 1000) {
          ticks_poll_device = SDL_GetTicks();
          if (run == WAIT_FOR_DEVICE && init_serial(0, preferred_device) == 1) {

            if (conf.audio_enabled == 1) {
              if (audio_init(conf.audio_buffer_size, conf.audio_device_name) == 0) {
                SDL_Log("Cannot initialize audio");
                conf.audio_enabled = 0;
              }
            }

            const int result = enable_and_reset_display();
            // Device was found; enable display and proceed to the main loop
            if (result == 1) {
              run = RUN;
              port_inited = 1;
              screensaver_destroy();
            } else {
              SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "Device not detected.");
              run = QUIT;
              screensaver_destroy();
            }
          }
        }
        SDL_Delay(conf.idle_ms);
      }
    } else {
      // classic startup behaviour, exit if device is not found
      if (port_inited == 0) {
        if (conf.audio_enabled == 1) {
          audio_destroy();
        }
        gamecontrollers_close();
        close_renderer();
        kill_inline_font();
        SDL_free(serial_buf);
        SDL_Quit();
        return -1;
      }
    }

    // main loop
    while (run == RUN) {

      // get current inputs
      const input_msg_s input = get_input_msg(&conf);

      switch (input.type) {
      case normal:
        if (input.value != prev_input) {
          prev_input = input.value;
          send_msg_controller(input.value);
        }
        break;
      case keyjazz:
        if (input.value != 0) {
          if (input.eventType == SDL_KEYDOWN && input.value != prev_input) {
            send_msg_keyjazz(input.value, input.value2);
            prev_note = input.value;
          } else if (input.eventType == SDL_KEYUP && input.value == prev_note) {
            send_msg_keyjazz(0xFF, 0);
          }
        }
        prev_input = input.value;
        break;
      case special:
        if (input.value != prev_input) {
          prev_input = input.value;
          switch (input.value) {
          case msg_quit:
            SDL_Log("Received msg_quit from input device.");
            run = 0;
            break;
          case msg_reset_display:
            reset_display();
            break;
          case msg_toggle_audio:
            toggle_audio(conf.audio_buffer_size, conf.audio_device_name);
            break;
          default:
            break;
          }
          break;
        }
      }

      while (1) {
        // read serial port
        const int bytes_read = serial_read(serial_buf, serial_read_size);
        if (bytes_read < 0) {
          SDL_LogCritical(SDL_LOG_CATEGORY_ERROR, "Error %d reading serial.", bytes_read);
          run = QUIT;
          break;
        }
        if (bytes_read > 0) {
          // input from device: reset the zero byte counter and create a
          // pointer to the serial buffer
          zerobyte_packets = 0;
          const uint8_t *cur = serial_buf;
          const uint8_t *end = serial_buf + bytes_read;
          while (cur < end) {
            // process the incoming bytes into commands and draw them
            const int n = slip_read_byte(&slip, *cur++);
            if (n != SLIP_NO_ERROR) {
              if (n == SLIP_ERROR_INVALID_PACKET) {
                reset_display();
              } else {
                SDL_LogError(SDL_LOG_CATEGORY_ERROR, "SLIP error %d\n", n);
              }
            }
          }
        } else {
          // zero byte packet, increment counter
          zerobyte_packets++;
#if defined(PS4) || defined(PS5)
          ps4_stage_once("run:first_idle");
#endif
          if (zerobyte_packets > conf.wait_packets) {
            zerobyte_packets = 0;

            // try opening the serial port to check if it's alive
            if (check_serial_port()) {
              // the device is still there, carry on
              break;
            }
            port_inited = 0;
            run = WAIT_FOR_DEVICE;
            close_serial_port();
            if (conf.audio_enabled == 1) {
              audio_destroy();
            }
            /* we'll make one more loop to see if the device is still there
             * but just sending zero bytes. if it doesn't get detected when
             * resetting the port, it will disconnect */
          }
          break;
        }
      }
      render_screen();
      SDL_Delay(conf.idle_ms);
    }
  } while (run > QUIT);
  // main loop end

  // exit, clean up
  SDL_Log("Shutting down\n");
  if (conf.audio_enabled == 1) {
    audio_destroy();
  }
  gamecontrollers_close();
  close_renderer();
  close_serial_port();
  SDL_free(serial_buf);
  SDL_Quit();
  return 0;
}
