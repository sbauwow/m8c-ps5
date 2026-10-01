// Compat header for m8c-ps5: the app sources are the PS4 port's copies with
// their #include "ps4_shims.h" / "usb_ps4.h" lines kept intact; on PS5 these
// redirect to the ps5_ implementations. KEEP THE PS4 API CONTRACT EXACT.
#ifndef PS4_SHIMS_H_
#define PS4_SHIMS_H_

#include "ps5_shims.h"

#define ps4_log_init ps5_log_init
#define ps4_stage ps5_stage
#define ps4_stage_once ps5_stage_once
#define ps4_logf ps5_logf
#define ps4_pref_path ps5_pref_path
#define ps4_notify ps5_notify
#define ps4_exit_to_home ps5_exit_to_home
#define ps4_audio_mode_name ps5_audio_mode_name

#endif
