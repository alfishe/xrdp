#if defined(HAVE_CONFIG_H)
#include "config_ac.h"
#endif

#include <check.h>

#include "sound_latency.h"

/* chansrv source PCM: 44.1 kHz, stereo, 16 bit */
#define BPS 176400
#define ALIGN 4
/* PipeWire hands over about 10 ms per message */
#define CHUNK 1764

struct run_result
{
    int chunks;             /* PCM chunks fed */
    int altered_chunks;     /* chunks that lost any bytes */
    int drop_events;        /* runs of consecutive altered chunks */
    long long dropped;      /* total bytes dropped */
};

/* Feed n chunks, one confirmation with the given latency every
 * confirm_every chunks, like a client confirming 46 ms wave blocks */
static void
feed(struct sound_latency *sl, int n, int confirm_every, int latency_ms,
     struct run_result *r)
{
    static int prev_altered = 0;
    int i;

    for (i = 0; i < n; i++)
    {
        int drop;

        if (i % confirm_every == 0)
        {
            sound_latency_confirm(sl, latency_ms);
        }
        drop = sound_latency_drop_bytes(sl, CHUNK, BPS, ALIGN);
        ck_assert_int_ge(drop, 0);
        ck_assert_int_le(drop, CHUNK);
        ck_assert_int_eq(drop % ALIGN, 0);
        r->chunks++;
        if (drop > 0)
        {
            r->altered_chunks++;
            r->dropped += drop;
            if (!prev_altered)
            {
                r->drop_events++;
            }
        }
        prev_altered = drop > 0;
    }
}

START_TEST(test_steady_latency_never_drops)
{
    struct sound_latency sl;
    struct run_result r = { 0 };

    sound_latency_reset(&sl);
    feed(&sl, 10000, 5, 90, &r);
    ck_assert_int_eq(r.altered_chunks, 0);
    ck_assert_int_eq(sl.best_ms, 90);
}
END_TEST

START_TEST(test_latency_rise_drops_once_by_excess)
{
    struct sound_latency sl;
    struct run_result r = { 0 };

    sound_latency_reset(&sl);
    feed(&sl, 500, 5, 90, &r);              /* best = 90 ms */
    ck_assert_int_eq(r.altered_chunks, 0);

    /* latency jumps to 400 ms and stays there until the drop lands */
    feed(&sl, 300, 5, 400, &r);
    /* then the client has caught up */
    feed(&sl, 5000, 5, 90, &r);

    /* Regression: the old code cut every chunk that followed a
     * confirmation to a quarter of silence for as long as the 50 sample
     * average stayed high, i.e. dozens of skips per second. */
    ck_assert_int_eq(r.drop_events, 1);
    /* the drop is the measured excess: the window average when it crossed
     * the threshold, so between the threshold and the full jump */
    ck_assert_int_ge(r.dropped,
                     (long long) SOUND_LATENCY_MAX_EXCESS_MS * BPS / 1000 - ALIGN);
    ck_assert_int_le(r.dropped, (long long) (400 - 90) * BPS / 1000 + ALIGN);
    ck_assert_int_eq(sl.best_ms, 90);
}
END_TEST

START_TEST(test_persistent_high_latency_waits_for_new_window)
{
    struct sound_latency sl;
    struct run_result r = { 0 };

    sound_latency_reset(&sl);
    feed(&sl, 500, 5, 90, &r);
    /* the client never drains: latency stays at 400 ms */
    feed(&sl, 5 * SOUND_LATENCY_WINDOW * 4, 5, 400, &r);

    /* one attempt to catch up, then 400 ms becomes the baseline instead
     * of a skip every window */
    ck_assert_int_eq(r.drop_events, 1);
    ck_assert_int_eq(sl.best_ms, 400);
}
END_TEST

/* Recorded on a live session: idle best 91 ms, while a video plays the
 * client keeps 250..360 ms. The old code skipped audio whenever the average
 * crossed best + 250, over and over. */
START_TEST(test_windows_app_video_latency)
{
    static const int pattern[] = { 351, 319, 296, 356, 312, 322, 332, 249,
                                   342, 292, 359, 351, 308, 286, 289, 261,
                                   295, 305, 313 };
    struct sound_latency sl;
    struct run_result r = { 0 };
    int i;

    sound_latency_reset(&sl);
    feed(&sl, 500, 5, 91, &r);
    /* each recorded value is a 50 block average sampled every ~5 s,
     * i.e. it held for about 100 confirmations */
    for (i = 0; i < 3 * (int) (sizeof(pattern) / sizeof(pattern[0])); i++)
    {
        feed(&sl, 500, 5, pattern[i % (sizeof(pattern) / sizeof(pattern[0]))],
             &r);
    }
    ck_assert_int_le(r.drop_events, 1);
    /* at most one catch up, never more than the largest excess seen */
    ck_assert_int_le(r.dropped, (long long) (359 - 91) * BPS / 1000 + ALIGN);
}
END_TEST

START_TEST(test_drop_spans_chunks)
{
    struct sound_latency sl;
    int i;
    long long total = 0;

    sound_latency_reset(&sl);
    sl.drop_ms = 300;                       /* 52920 bytes, 30 chunks */
    for (i = 0; i < 100; i++)
    {
        total += sound_latency_drop_bytes(&sl, CHUNK, BPS, ALIGN);
    }
    ck_assert_int_eq(sl.drop_ms, 0);
    ck_assert_int_ge(total, 300LL * BPS / 1000 - CHUNK);
    ck_assert_int_le(total, 300LL * BPS / 1000 + ALIGN);
}
END_TEST

START_TEST(test_small_rise_below_threshold_ignored)
{
    struct sound_latency sl;
    struct run_result r = { 0 };

    sound_latency_reset(&sl);
    feed(&sl, 500, 5, 90, &r);
    feed(&sl, 5000, 5, 90 + SOUND_LATENCY_MAX_EXCESS_MS - 10, &r);
    ck_assert_int_eq(r.altered_chunks, 0);
}
END_TEST

START_TEST(test_reset_forgets_best)
{
    struct sound_latency sl;
    struct run_result r = { 0 };

    sound_latency_reset(&sl);
    feed(&sl, 500, 5, 90, &r);
    sound_latency_reset(&sl);
    /* new playback with a higher base latency is not a rise */
    feed(&sl, 5000, 5, 400, &r);
    ck_assert_int_eq(r.altered_chunks, 0);
    ck_assert_int_eq(sl.best_ms, 400);
}
END_TEST

START_TEST(test_current_latency)
{
    struct sound_latency sl;
    int i;

    sound_latency_reset(&sl);
    ck_assert_int_eq(sound_latency_current_ms(&sl), 0);
    /* partial window: average of what was measured so far */
    sound_latency_confirm(&sl, 100);
    sound_latency_confirm(&sl, 200);
    ck_assert_int_eq(sound_latency_current_ms(&sl), 150);
    /* full window rolls over the oldest samples */
    for (i = 0; i < SOUND_LATENCY_WINDOW; i++)
    {
        sound_latency_confirm(&sl, 120);
    }
    ck_assert_int_eq(sound_latency_current_ms(&sl), 120);
}
END_TEST

START_TEST(test_current_latency_follows_client_buffering)
{
    struct sound_latency sl;
    int i;

    /* idle, then video makes the client buffer about 300 ms: the reported
     * latency must end up there, also across the catch up restart */
    sound_latency_reset(&sl);
    for (i = 0; i < 100; i++)
    {
        sound_latency_confirm(&sl, 90);
    }
    ck_assert_int_eq(sound_latency_current_ms(&sl), 90);
    for (i = 0; i < 200; i++)
    {
        sound_latency_confirm(&sl, 400);
        sound_latency_drop_bytes(&sl, CHUNK, BPS, ALIGN);
        ck_assert_int_ge(sound_latency_current_ms(&sl), 90);
        ck_assert_int_le(sound_latency_current_ms(&sl), 400);
    }
    ck_assert_int_eq(sound_latency_current_ms(&sl), 400);
}
END_TEST

START_TEST(test_sink_latency_subtracts_video)
{
    /* nothing measured yet */
    ck_assert_int_eq(sound_latency_sink_ms(0, 23, 40), 0);
    /* audio only */
    ck_assert_int_eq(sound_latency_sink_ms(300, 23, 0), 323);
    /* players delay video by the difference */
    ck_assert_int_eq(sound_latency_sink_ms(300, 23, 60), 263);
    /* video slower than audio: smallest latency */
    ck_assert_int_eq(sound_latency_sink_ms(50, 0, 80), 1);
    ck_assert_int_eq(sound_latency_sink_ms(80, 0, 80), 1);
    /* bad inputs */
    ck_assert_int_eq(sound_latency_sink_ms(300, -5, -7), 300);
}
END_TEST

START_TEST(test_bad_arguments)
{
    struct sound_latency sl;

    sound_latency_reset(&sl);
    sl.drop_ms = 100;
    ck_assert_int_eq(sound_latency_drop_bytes(&sl, 0, BPS, ALIGN), 0);
    ck_assert_int_eq(sound_latency_drop_bytes(&sl, CHUNK, 0, ALIGN), 0);
    ck_assert_int_eq(sound_latency_drop_bytes(&sl, CHUNK, BPS, 0), 0);
    ck_assert_int_eq(sound_latency_drop_bytes(&sl, 2, BPS, ALIGN), 0);
    ck_assert_int_eq(sl.drop_ms, 100);
}
END_TEST

Suite *
make_suite_test_sound_latency(void)
{
    Suite *s = suite_create("SoundLatency");
    TCase *tc = tcase_create("sound_latency");

    tcase_add_test(tc, test_steady_latency_never_drops);
    tcase_add_test(tc, test_latency_rise_drops_once_by_excess);
    tcase_add_test(tc, test_persistent_high_latency_waits_for_new_window);
    tcase_add_test(tc, test_windows_app_video_latency);
    tcase_add_test(tc, test_drop_spans_chunks);
    tcase_add_test(tc, test_small_rise_below_threshold_ignored);
    tcase_add_test(tc, test_reset_forgets_best);
    tcase_add_test(tc, test_current_latency);
    tcase_add_test(tc, test_current_latency_follows_client_buffering);
    tcase_add_test(tc, test_sink_latency_subtracts_video);
    tcase_add_test(tc, test_bad_arguments);
    suite_add_tcase(s, tc);
    return s;
}
