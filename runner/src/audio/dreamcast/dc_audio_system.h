/* AICA audio backend for Butterscotch on the Sega Dreamcast.
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef _BS_DC_AUDIO_SYSTEM_H_
#define _BS_DC_AUDIO_SYSTEM_H_

#include "audio_system.h"

#define DC_SOUND_INSTANCE_ID_BASE 100000
#define DC_AUDIO_STREAM_INDEX_BASE 300000

/* audioPath: AUDIO.BIN produced by the builder (e.g. "/cd/AUDIO.BIN") */
AudioSystem* DcAudioSystem_create(const char* audioPath);

#endif
