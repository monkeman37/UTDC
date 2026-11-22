/* Save-file system for the Dreamcast port.
 *
 * GameMaker games write a handful of small text files (Undertale: file0,
 * file9, undertale.ini, config.ini). They live in RAM and are packed into a
 * single compressed VMU file so they show up as one save in the Dreamcast
 * memory-card manager. Files that aren't saves are read from the disc.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef _BS_DC_FILE_SYSTEM_H_
#define _BS_DC_FILE_SYSTEM_H_

#include "file_system.h"
#include <stdbool.h>

FileSystem* DcFileSystem_create(const char* gameTitle, const char* discRoot);

/* Writes pending changes to the VMU if any (call once per frame; it rate-limits itself). */
void DcFileSystem_tick(FileSystem* fs);

/* Forces a VMU write now if anything changed. Returns false if no VMU was found. */
bool DcFileSystem_flush(FileSystem* fs);

/* Blocks until a background VMU write in progress has finished (call before exiting). */
void DcFileSystem_waitIdle(FileSystem* fs);

/* True while there are saves that couldn't be written (no VMU / VMU full). */
bool DcFileSystem_hasUnsavedData(FileSystem* fs);
const char* DcFileSystem_status(FileSystem* fs);

#endif
