/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef _BS_DC_TEXT_PATCHES_H_
#define _BS_DC_TEXT_PATCHES_H_

#include "data_win.h"

/* Swaps PC keyboard prompts in the string table for Dreamcast button names.
   Returns the number of strings changed. Call after DataWin_parse, before VM_create. */
int DcTextPatches_apply(DataWin* dw);

/* For ds_map_add: returns the PC text key that should also receive this console key's text. */
const char* DcTextPatches_aliasFor(const char* key);

#endif
