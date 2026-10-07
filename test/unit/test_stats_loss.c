/**
 * @file test_stats_loss.c  TX loss windowed on receiver reports
 *
 * Replays the sequence from the 2026-10-07 echo-test capture: Asterisk
 * reports every 5 s (250 packets), the SDK polls every 2 s, and the
 * cumulative lost count moved 32 -> 40 between two reports while the
 * percentage read 0%.  No external dependencies: includes the header.
 */

#include <stdio.h>
#include <math.h>
#include <string.h>
#include "../../src/stats_loss.h"

static int g_pass, g_fail;

#define CHECK(cond, ...) do {                                           \
	if (cond) { g_pass++; }                                         \
	else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__);    \
	       printf(__VA_ARGS__); printf("\n"); }                     \
} while (0)

static int near(float a, float b) { return fabsf(a - b) < 0.01f; }

static void test_no_report_yet(void)
{
	struct bsdk_tx_loss_win w = {0};
	CHECK(!bsdk_tx_loss_fold(&w, 0, 0, 100), "ehsn 0 is no report");
	CHECK(!w.seeded && w.pct == 0.f, "nothing folded");
}

static void test_first_report_uses_sent(void)
{
	struct bsdk_tx_loss_win w = {0};
	/* First RR after 250 sent, 8 lost: the whole stream is the window. */
	CHECK(bsdk_tx_loss_fold(&w, 70000 + 250, 8, 250), "first RR folds");
	CHECK(near(w.pct, 3.2f), "first RR 8/250 = 3.2%%, got %f", w.pct);
}

static void test_report_interval_holds_between_reports(void)
{
	struct bsdk_tx_loss_win w = {0};
	uint32_t base = 70000;
	bsdk_tx_loss_fold(&w, base + 250, 32, 250);

	/* Two 2 s ticks before the next RR: same report, figure held. */
	CHECK(!bsdk_tx_loss_fold(&w, base + 250, 32, 350), "tick 1: no new RR");
	CHECK(!bsdk_tx_loss_fold(&w, base + 250, 32, 450), "tick 2: no new RR");
	CHECK(near(w.pct, 12.8f), "held at 32/250, got %f", w.pct);

	/* The capture's moment: lost 32 -> 40 over the next 250 packets. */
	CHECK(bsdk_tx_loss_fold(&w, base + 500, 40, 500), "RR 2 folds");
	CHECK(near(w.pct, 3.2f), "8 lost of 250 = 3.2%%, not 0%%; got %f", w.pct);
	CHECK(!bsdk_tx_loss_fold(&w, base + 500, 40, 600), "tick after: held");
	CHECK(near(w.pct, 3.2f), "still 3.2%%, got %f", w.pct);

	/* A clean interval reads 0, and only then. */
	CHECK(bsdk_tx_loss_fold(&w, base + 750, 40, 750), "RR 3 folds");
	CHECK(w.pct == 0.f, "no new loss = 0%%, got %f", w.pct);
}

static void test_duplicates_never_go_negative(void)
{
	struct bsdk_tx_loss_win w = {0};
	bsdk_tx_loss_fold(&w, 1250, 10, 250);
	/* Cumulative lost falls when duplicates arrive (RFC 3550 A.3). */
	CHECK(bsdk_tx_loss_fold(&w, 1500, 7, 500), "RR folds");
	CHECK(w.pct == 0.f, "negative delta clamps to 0, got %f", w.pct);
}

static void test_lost_clamped_to_expected(void)
{
	struct bsdk_tx_loss_win w = {0};
	bsdk_tx_loss_fold(&w, 1250, 0, 250);
	CHECK(bsdk_tx_loss_fold(&w, 1260, 1000, 260), "RR folds");
	CHECK(near(w.pct, 100.f), "more lost than expected = 100%%, got %f", w.pct);
}

static void test_counter_reset_reseeds(void)
{
	struct bsdk_tx_loss_win w = {0};
	bsdk_tx_loss_fold(&w, 90000, 50, 1000);
	/* Peer restarted its counters: ehsn went backwards. */
	CHECK(bsdk_tx_loss_fold(&w, 300, 3, 300), "reset folds");
	CHECK(near(w.pct, 1.0f), "reseeded from the whole stream 3/300, got %f",
	      w.pct);
	CHECK(w.prev_ehsn == 300, "baseline moved to the new counters");
}

static void test_no_packets_sent_in_interval(void)
{
	struct bsdk_tx_loss_win w = {0};
	bsdk_tx_loss_fold(&w, 1000, 5, 0);
	CHECK(w.pct == 0.f, "nothing sent yet: 0%%, got %f", w.pct);
}

static void test_rx_snapshot_window(void)
{
	struct bsdk_rx_loss_win w = {0};
	CHECK(!bsdk_rx_loss_fold(&w, 0, 0), "no snapshot yet");

	/* First snapshot: 230 received, 21 lost. */
	CHECK(bsdk_rx_loss_fold(&w, 230, 21), "first snapshot folds");
	CHECK(near(w.pct, 8.37f), "21/251 = 8.37%%, got %f", w.pct);

	/* The 2 s ticks between RTCP packets see the same snapshot. */
	CHECK(!bsdk_rx_loss_fold(&w, 230, 21), "same snapshot: held");
	CHECK(near(w.pct, 8.37f), "still 8.37%%, got %f", w.pct);

	/* Next snapshot: +230 received, +21 lost — the capture's 1-in-12. */
	CHECK(bsdk_rx_loss_fold(&w, 460, 42), "snapshot 2 folds");
	CHECK(near(w.pct, 8.37f), "8.37%%, not 0%%; got %f", w.pct);

	/* Clean interval, then a reset of the peer's counters. */
	CHECK(bsdk_rx_loss_fold(&w, 710, 42), "snapshot 3 folds");
	CHECK(w.pct == 0.f, "no new loss = 0%%, got %f", w.pct);
	CHECK(bsdk_rx_loss_fold(&w, 100, 5), "reset folds");
	CHECK(near(w.pct, 4.76f), "reseeded 5/105, got %f", w.pct);

	/* Duplicates: the lost count falls. */
	CHECK(bsdk_rx_loss_fold(&w, 200, 2), "folds");
	CHECK(w.pct == 0.f, "negative delta clamps to 0, got %f", w.pct);
}

int main(void)
{
	test_rx_snapshot_window();
	test_no_report_yet();
	test_first_report_uses_sent();
	test_report_interval_holds_between_reports();
	test_duplicates_never_go_negative();
	test_lost_clamped_to_expected();
	test_counter_reset_reseeds();
	test_no_packets_sent_in_interval();
	printf("%s: %d passed, %d failed\n", __FILE__, g_pass, g_fail);
	return g_fail ? 1 : 0;
}
