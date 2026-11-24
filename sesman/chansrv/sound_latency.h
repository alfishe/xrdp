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
 * @file sound_latency.h
 * @brief Playback latency control for the rdpsnd output channel
 *
 * The client confirms each wave block when it has played it, so the time
 * from sending a block to its confirmation is the client side playback
 * latency. If that latency grows well beyond the best seen, audio is
 * dropped once by the excess to catch up, and the measurement window
 * starts over. If the next full window shows the latency did not come
 * down, the client simply buffers more (Windows App keeps about 300 ms
 * while video plays), so that latency becomes the new baseline instead of
 * dropping again. A latency rise therefore costs at most one skip.
 */

#ifndef SOUND_LATENCY_H
#define SOUND_LATENCY_H

/** Confirmations averaged before a decision is made */
#define SOUND_LATENCY_WINDOW 50
/** Latency above the best that triggers a catch up drop, ms */
#define SOUND_LATENCY_MAX_EXCESS_MS 250

struct sound_latency
{
    int window[SOUND_LATENCY_WINDOW];
    int count;
    int next;
    int best_ms;            /* lowest window average seen, 0 = none yet */
    int last_avg_ms;        /* last full window average, 0 = none yet */
    int drop_ms;            /* excess still to be dropped */
    int verify;             /* next full window checks the last drop */
};

/**
 * Forget all measurements, e.g. when playback is closed
 */
void
sound_latency_reset(struct sound_latency *sl);

/**
 * Record the latency of one confirmed wave block
 * @param sl latency state
 * @param latency_ms time from sending the block to its confirmation
 */
void
sound_latency_confirm(struct sound_latency *sl, int latency_ms);

/**
 * Bytes to drop from the start of the next PCM data
 *
 * Call for every PCM chunk before it is buffered. The caller skips the
 * returned number of bytes and sends the rest unchanged.
 *
 * @param sl latency state
 * @param data_bytes size of the PCM chunk
 * @param bytes_per_sec PCM byte rate
 * @param block_align PCM frame size in bytes
 * @return bytes to skip, a multiple of block_align, <= data_bytes
 */
int
sound_latency_drop_bytes(struct sound_latency *sl, int data_bytes,
                         int bytes_per_sec, int block_align);

#endif
