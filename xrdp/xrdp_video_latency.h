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
 * @file xrdp_video_latency.h
 * @brief Video latency as seen by the client
 *
 * The time from a frame being queued for encoding to the client
 * acknowledging it, which it does once the frame is decoded. chansrv
 * subtracts it from the audio latency it reports to the sound server, so
 * players delay video by the difference and picture and sound meet.
 */

#ifndef XRDP_VIDEO_LATENCY_H
#define XRDP_VIDEO_LATENCY_H

/** Frames in flight that can be matched to their acknowledgement */
#define VIDEO_LATENCY_SLOTS 64
/** Acknowledged frames the latency is the median of */
#define VIDEO_LATENCY_WINDOW 31

struct video_latency
{
    int slot_id[VIDEO_LATENCY_SLOTS];
    int slot_time[VIDEO_LATENCY_SLOTS];
    int slot_used[VIDEO_LATENCY_SLOTS];
    int window[VIDEO_LATENCY_WINDOW];
    int count;
    int next;
};

/**
 * Forget all frames and measurements
 */
void
video_latency_reset(struct video_latency *vl);

/**
 * Record a frame sent to the client
 * @param vl latency state
 * @param frame_id frame id as sent in the end frame PDU
 * @param queued_ms time the frame was queued for encoding, g_time3()
 */
void
video_latency_frame_sent(struct video_latency *vl, int frame_id,
                         int queued_ms);

/**
 * Record a frame acknowledgement from the client
 * @param vl latency state
 * @param frame_id acknowledged frame id
 * @param now_ms current time, g_time3()
 * @return 1 if the frame was known and measured, else 0
 */
int
video_latency_frame_acked(struct video_latency *vl, int frame_id,
                          int now_ms);

/**
 * Current video latency
 * @return median over the last acknowledged frames in ms, 0 if none yet
 */
int
video_latency_ms(const struct video_latency *vl);

#endif
