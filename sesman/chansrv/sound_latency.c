/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Copyright (C) 2026 alfishe
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file sound_latency.c
 * @brief Playback latency control for the rdpsnd output channel
 */

#if defined(HAVE_CONFIG_H)
#include <config_ac.h>
#endif

#include <string.h>

#include "sound_latency.h"

/*****************************************************************************/
void
sound_latency_reset(struct sound_latency *sl)
{
    memset(sl, 0, sizeof(*sl));
}

/*****************************************************************************/
void
sound_latency_confirm(struct sound_latency *sl, int latency_ms)
{
    int index;
    int sum;
    int avg;

    sl->window[sl->next] = latency_ms;
    sl->next = (sl->next + 1) % SOUND_LATENCY_WINDOW;
    if (sl->count < SOUND_LATENCY_WINDOW)
    {
        sl->count++;
    }
    if (sl->count < SOUND_LATENCY_WINDOW)
    {
        return;
    }

    sum = 0;
    for (index = 0; index < SOUND_LATENCY_WINDOW; index++)
    {
        sum += sl->window[index];
    }
    avg = sum / SOUND_LATENCY_WINDOW;
    sl->last_avg_ms = avg;
    if (sl->best_ms < 1 || avg < sl->best_ms)
    {
        sl->best_ms = avg;
    }
    if (sl->verify)
    {
        /* first full window after a drop */
        sl->verify = 0;
        if (avg > sl->best_ms + SOUND_LATENCY_MAX_EXCESS_MS)
        {
            /* dropping did not help: the client buffers this much */
            sl->best_ms = avg;
        }
        return;
    }
    if (sl->drop_ms == 0 && avg > sl->best_ms + SOUND_LATENCY_MAX_EXCESS_MS)
    {
        /* Drop the excess once, then measure again from scratch: blocks
         * sent before the drop still carry the old latency */
        sl->drop_ms = avg - sl->best_ms;
        sl->verify = 1;
        sl->count = 0;
        sl->next = 0;
    }
}

/*****************************************************************************/
int
sound_latency_drop_bytes(struct sound_latency *sl, int data_bytes,
                         int bytes_per_sec, int block_align)
{
    long long want;
    int drop;

    if (sl->drop_ms <= 0 || data_bytes <= 0 || bytes_per_sec <= 0 ||
            block_align <= 0)
    {
        return 0;
    }
    want = (long long) sl->drop_ms * bytes_per_sec / 1000;
    drop = (want < data_bytes) ? (int) want : data_bytes;
    drop -= drop % block_align;
    /* account for what was dropped, in ms, rounding up so it ends */
    if (drop >= want)
    {
        sl->drop_ms = 0;
    }
    else
    {
        int dropped_ms = (int) (((long long) drop * 1000 + bytes_per_sec - 1) /
                                bytes_per_sec);
        sl->drop_ms = (dropped_ms >= sl->drop_ms) ? 0 : sl->drop_ms - dropped_ms;
    }
    return drop;
}
