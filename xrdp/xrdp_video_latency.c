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
 * @file xrdp_video_latency.c
 * @brief Video latency as seen by the client
 */

#if defined(HAVE_CONFIG_H)
#include <config_ac.h>
#endif

#include <stdlib.h>
#include <string.h>

#include "xrdp_video_latency.h"

/*****************************************************************************/
void
video_latency_reset(struct video_latency *vl)
{
    memset(vl, 0, sizeof(*vl));
}

/*****************************************************************************/
static int
slot_of(int frame_id)
{
    return (int) ((unsigned int) frame_id % VIDEO_LATENCY_SLOTS);
}

/*****************************************************************************/
void
video_latency_frame_sent(struct video_latency *vl, int frame_id,
                         int queued_ms)
{
    int slot = slot_of(frame_id);

    vl->slot_id[slot] = frame_id;
    vl->slot_time[slot] = queued_ms;
    vl->slot_used[slot] = 1;
}

/*****************************************************************************/
int
video_latency_frame_acked(struct video_latency *vl, int frame_id,
                          int now_ms)
{
    int slot = slot_of(frame_id);
    int latency;

    if (!vl->slot_used[slot] || vl->slot_id[slot] != frame_id)
    {
        /* unknown, already measured, or a catch all acknowledgement */
        return 0;
    }
    vl->slot_used[slot] = 0;
    latency = now_ms - vl->slot_time[slot];
    if (latency < 0)
    {
        return 0;
    }
    vl->window[vl->next] = latency;
    vl->next = (vl->next + 1) % VIDEO_LATENCY_WINDOW;
    if (vl->count < VIDEO_LATENCY_WINDOW)
    {
        vl->count++;
    }
    return 1;
}

/*****************************************************************************/
static int
cmp_int(const void *a, const void *b)
{
    int x = *(const int *) a;
    int y = *(const int *) b;

    return (x > y) - (x < y);
}

/*****************************************************************************/
int
video_latency_ms(const struct video_latency *vl)
{
    int sorted[VIDEO_LATENCY_WINDOW];

    if (vl->count == 0)
    {
        return 0;
    }
    /* the first count entries are valid until the window wraps */
    memcpy(sorted, vl->window, sizeof(int) * vl->count);
    qsort(sorted, vl->count, sizeof(int), cmp_int);
    return sorted[vl->count / 2];
}
