/**
 * @file stats_loss.h  TX loss rate, windowed on the peer's receiver reports
 *
 * RTCP receiver reports carry a lifetime cumulative lost count (RFC 3550
 * A.3) and arrive on the peer's schedule — every 5 s from Asterisk — while
 * the stats poll runs every stats_interval_ms (2 s).  And baresip refreshes
 * its whole rtcp_stats snapshot, the RX side included, only when an RTCP
 * packet arrives (src/stream.c rtcp_handler), so every counter the poll
 * reads moves in steps on the peer's schedule.  Differenced over the poll
 * window, both loss rates read 0% on every tick that saw no new report and
 * the real figure on the one that did, and the MOS swung between its
 * ceiling and the truth six times a minute.
 *
 * So the window is the report interval.  TX: the packets lost over the
 * sequence range the report covers (the delta of its extended highest
 * sequence).  RX: lost over received + lost since the snapshot last moved.
 * Either figure holds until the next report.
 *
 * Free of re/baresip so test/unit/test_stats_loss.c can include it as is.
 */
#ifndef ECHOSDK_STATS_LOSS_H
#define ECHOSDK_STATS_LOSS_H

#include <stdint.h>
#include <stdbool.h>

struct bsdk_tx_loss_win {
	bool     seeded;     /**< a receiver report has been folded in        */
	uint32_t prev_ehsn;  /**< extended highest sequence in the last report */
	int32_t  prev_lost;  /**< cumulative lost count in the last report     */
	float    pct;        /**< loss over the last report interval, held     */
};

/**
 * Fold the latest receiver-report state in.
 *
 * @param w     window state, zero-initialised at call start
 * @param ehsn  the peer's extended highest sequence received; 0 until the
 *              first report
 * @param lost  the peer's cumulative lost count (signed: RFC 3550 A.3)
 * @param sent  our own cumulative packets sent, the only denominator on
 *              hand for the first report
 *
 * @return true when a new report was folded and w->pct recomputed; false
 *         when there is no report yet or the last one is already in, in
 *         which case w->pct is unchanged.
 */
static inline bool bsdk_tx_loss_fold(struct bsdk_tx_loss_win *w,
                                     uint32_t ehsn, int32_t lost,
                                     uint32_t sent)
{
	uint32_t expected;
	int32_t  dlost;

	if (!ehsn)
		return false;                      /* no report yet */
	if (w->seeded && ehsn == w->prev_ehsn)
		return false;                      /* same report as last time */

	if (!w->seeded || ehsn < w->prev_ehsn) {
		/* First report, or the peer's counters restarted under us
		 * (SSRC change, stream restart): the only interval on hand
		 * is the whole stream so far. */
		expected = sent;
		dlost    = lost;
	}
	else {
		expected = ehsn - w->prev_ehsn;
		dlost    = lost - w->prev_lost;
	}

	/* Duplicates after a reordering burst make the cumulative count
	 * fall (RFC 3550 A.3): nothing was lost in that interval. */
	if (dlost < 0)
		dlost = 0;
	if ((uint32_t)dlost > expected)
		dlost = (int32_t)expected;

	w->pct       = expected ? (float)dlost * 100.f / (float)expected : 0.f;
	w->prev_ehsn = ehsn;
	w->prev_lost = lost;
	w->seeded    = true;
	return true;
}

struct bsdk_rx_loss_win {
	bool     seeded;     /**< a snapshot has been folded in               */
	uint32_t prev_recv;  /**< packets received in the last snapshot        */
	int32_t  prev_lost;  /**< cumulative lost count in the last snapshot   */
	float    pct;        /**< loss over the last report interval, held     */
};

/**
 * Fold the latest RX snapshot in.
 *
 * @param w     window state, zero-initialised at call start
 * @param recv  packets received from the peer, as of the snapshot
 * @param lost  packets lost from the peer, cumulative (signed: RFC 3550 A.3)
 *
 * @return true when the snapshot had moved and w->pct was recomputed.
 */
static inline bool bsdk_rx_loss_fold(struct bsdk_rx_loss_win *w,
                                     uint32_t recv, int32_t lost)
{
	uint32_t drecv, expected;
	int32_t  dlost;

	if (!recv && !lost)
		return false;                      /* no snapshot yet */
	if (w->seeded && recv == w->prev_recv && lost == w->prev_lost)
		return false;                      /* snapshot has not moved */

	if (!w->seeded || recv < w->prev_recv) {
		drecv = recv;                      /* first, or counters reset */
		dlost = lost;
	}
	else {
		drecv = recv - w->prev_recv;
		dlost = lost - w->prev_lost;
	}

	if (dlost < 0)
		dlost = 0;
	expected = drecv + (uint32_t)dlost;

	w->pct       = expected ? (float)dlost * 100.f / (float)expected : 0.f;
	w->prev_recv = recv;
	w->prev_lost = lost;
	w->seeded    = true;
	return true;
}

#endif /* ECHOSDK_STATS_LOSS_H */
