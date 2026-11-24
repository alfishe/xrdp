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

#if defined(HAVE_CONFIG_H)
#include "config_ac.h"
#endif

#include "xrdp_video_latency.h"
#include "test_xrdp.h"

START_TEST(test_no_frames_no_latency)
{
    struct video_latency vl;

    video_latency_reset(&vl);
    ck_assert_int_eq(video_latency_ms(&vl), 0);
    ck_assert_int_eq(video_latency_frame_acked(&vl, 1, 100), 0);
    ck_assert_int_eq(video_latency_ms(&vl), 0);
}
END_TEST

START_TEST(test_ack_measures_queue_to_ack)
{
    struct video_latency vl;
    int id;

    video_latency_reset(&vl);
    for (id = 1; id <= 100; id++)
    {
        video_latency_frame_sent(&vl, id, id * 10);
        ck_assert_int_eq(video_latency_frame_acked(&vl, id, id * 10 + 45), 1);
    }
    ck_assert_int_eq(video_latency_ms(&vl), 45);
}
END_TEST

START_TEST(test_frames_in_flight)
{
    struct video_latency vl;
    int id;

    /* 8 frames queued before the first ack comes back */
    video_latency_reset(&vl);
    for (id = 0; id < 8; id++)
    {
        video_latency_frame_sent(&vl, id, id * 8);
    }
    for (id = 0; id < 8; id++)
    {
        ck_assert_int_eq(video_latency_frame_acked(&vl, id, id * 8 + 70), 1);
    }
    ck_assert_int_eq(video_latency_ms(&vl), 70);
}
END_TEST

START_TEST(test_ack_once_and_catch_all_ignored)
{
    struct video_latency vl;

    video_latency_reset(&vl);
    video_latency_frame_sent(&vl, 5, 1000);
    ck_assert_int_eq(video_latency_frame_acked(&vl, 5, 1030), 1);
    /* repeated ack, catch all ids and acks from before a wrap of the
     * slots are not measured */
    ck_assert_int_eq(video_latency_frame_acked(&vl, 5, 1500), 0);
    ck_assert_int_eq(video_latency_frame_acked(&vl, -1, 1500), 0);
    ck_assert_int_eq(video_latency_frame_acked(&vl, 0x7fffffff, 1500), 0);
    video_latency_frame_sent(&vl, 6, 1000);
    video_latency_frame_sent(&vl, 6 + VIDEO_LATENCY_SLOTS, 2000);
    ck_assert_int_eq(video_latency_frame_acked(&vl, 6, 2010), 0);
    ck_assert_int_eq(video_latency_ms(&vl), 30);
}
END_TEST

START_TEST(test_median_ignores_stalls)
{
    struct video_latency vl;
    int id;

    video_latency_reset(&vl);
    for (id = 0; id < VIDEO_LATENCY_WINDOW; id++)
    {
        int lat = (id % 5 == 0) ? 900 : 50;
        video_latency_frame_sent(&vl, id, 0);
        video_latency_frame_acked(&vl, id, lat);
    }
    ck_assert_int_eq(video_latency_ms(&vl), 50);
}
END_TEST

START_TEST(test_follows_change)
{
    struct video_latency vl;
    int id;

    video_latency_reset(&vl);
    for (id = 0; id < 200; id++)
    {
        video_latency_frame_sent(&vl, id, id * 10);
        video_latency_frame_acked(&vl, id, id * 10 + (id < 100 ? 40 : 120));
    }
    ck_assert_int_eq(video_latency_ms(&vl), 120);
}
END_TEST

Suite *
make_suite_video_latency(void)
{
    Suite *s = suite_create("video_latency");
    TCase *tc = tcase_create("video_latency");

    tcase_add_test(tc, test_no_frames_no_latency);
    tcase_add_test(tc, test_ack_measures_queue_to_ack);
    tcase_add_test(tc, test_frames_in_flight);
    tcase_add_test(tc, test_ack_once_and_catch_all_ignored);
    tcase_add_test(tc, test_median_ignores_stalls);
    tcase_add_test(tc, test_follows_change);
    suite_add_tcase(s, tc);
    return s;
}
