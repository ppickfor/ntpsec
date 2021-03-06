/*
 * systime -- routines to fiddle a UNIX clock.
 *
 * ATTENTION: Get approval from Dave Mills on all changes to this file!
 *
 */
#include <config.h>

#include "ntp.h"
#include "ntp_syslog.h"
#include "ntp_stdlib.h"
#include "ntp_random.h"
#include "timespecops.h"
#include "ntp_calendar.h"

#ifdef HAVE_UTMPX_H
#include <utmpx.h>
#endif

#ifndef USE_COMPILETIME_PIVOT
# define USE_COMPILETIME_PIVOT 1
#endif

/*
 * These routines (get_systime, step_systime, adj_systime) implement an
 * interface between the system independent NTP clock and the Unix
 * system clock in various architectures and operating systems. Time is
 * a precious quantity in these routines and every effort is made to
 * minimize errors by unbiased rounding and amortizing adjustment
 * residues.
 *
 * In order to improve the apparent resolution, provide unbiased
 * rounding and most importantly ensure that the readings cannot be
 * predicted, the low-order unused portion of the time below the minimum
 * time to read the clock is filled with an unbiased random fuzz.
 *
 * The sys_tick variable specifies the system clock tick interval in
 * seconds, for stepping clocks, defined as those which return times
 * less than MINSTEP greater than the previous reading. For systems that
 * use a high-resolution counter such that each clock reading is always
 * at least MINSTEP greater than the prior, sys_tick is the time to read
 * the system clock.
 *
 * The sys_fuzz variable measures the minimum time to read the system
 * clock, regardless of its precision.  When reading the system clock
 * using get_systime() after sys_tick and sys_fuzz have been determined,
 * ntpd ensures each unprocessed clock reading is no less than sys_fuzz
 * later than the prior unprocessed reading, and then fuzzes the bits
 * below sys_fuzz in the timestamp returned, ensuring each of its
 * resulting readings is strictly later than the previous.
 *
 * When slewing the system clock using adj_systime() (with the kernel
 * loop discipline unavailable or disabled), adjtime() offsets are
 * quantized to sys_tick, if sys_tick is greater than sys_fuzz, which
 * is to say if the OS presents a stepping clock.  Otherwise, offsets
 * are quantized to the microsecond resolution of adjtime()'s timeval
 * input.  The remaining correction sys_residual is carried into the
 * next adjtime() and meanwhile is also factored into get_systime()
 * readings.
 *
 * adj_systime() and step_systime() will behave sanely with these
 * variables not set, but the adjustments may be in larger steps.
 */
double	sys_tick = 0;		/* tick size or time to read (s) */
double	sys_fuzz = 0;		/* min. time to read the clock (s) */
bool	trunc_os_clock;		/* sys_tick > measured_tick */
time_stepped_callback	step_callback;

static double	sys_residual = 0;	/* adjustment residue (s) */
static long	sys_fuzz_nsec = 0;	/* min. time to read the clock (ns) */

/* perlinger@ntp.org: As 'get_systime()' does its own check for clock
 * backstepping, this could probably become a local variable in
 * 'get_systime()' and the cruft associated with communicating via a
 * static value could be removed after the v4.2.8 release.
 */
static bool lamport_violated;	/* clock was stepped back */

void
set_sys_fuzz(
	double	fuzz_val
	)
{
	sys_fuzz = fuzz_val;
	//INSIST(sys_fuzz >= 0);
	//INSIST(sys_fuzz <= 1.0);
	sys_fuzz_nsec = (long)(sys_fuzz * 1e9 + 0.5);
}


void
get_ostime(
	struct timespec *	tsp
	)
{
	int	rc;
	long	ticks;

	rc = clock_gettime(CLOCK_REALTIME, tsp);
	if (rc < 0) {
#ifndef __COVERITY__
		msyslog(LOG_ERR, "read system clock failed: %m (%d)",
			errno);
#endif /* __COVERITY__ */
		exit(1);
	}

	if (trunc_os_clock) {
		ticks = (long)((tsp->tv_nsec * 1e-9) / sys_tick);
		tsp->tv_nsec = (long)(ticks * 1e9 * sys_tick);
	}
}


/*
 * get_systime - return system time in NTP timestamp format.
 */
void
get_systime(
	l_fp *now		/* system time */
	)
{
	struct timespec ts;	/* seconds and nanoseconds */
	get_ostime(&ts);
	normalize_time(ts, sys_fuzz > 0.0 ? ntp_random() : 0, now);
}

void
normalize_time(
	struct timespec ts,		/* seconds and nanoseconds */
	long rand,
	l_fp *now		/* system time */
	)
{
        static struct timespec  ts_last;        /* last sampled os time */
	static struct timespec	ts_prev;	/* prior os time */
	static l_fp		lfp_prev;	/* prior result */
	static double		dfuzz_prev;	/* prior fuzz */
	struct timespec ts_min;	/* earliest permissible */
	struct timespec ts_lam;	/* lamport fictional increment */
	struct timespec ts_prev_log;	/* for msyslog only */
	double	dfuzz;
	double	ddelta;
	l_fp	result;
	l_fp	lfpfuzz;
	l_fp	lfpdelta;

        /* First check if there was a Lamport violation, that is, two
         * successive calls to 'get_ostime()' resulted in negative
         * time difference. Use a few milliseconds of permissible
         * tolerance -- being too sharp can hurt here. (This is intended
         * for the Win32 target, where the HPC interpolation might
         * introduce small steps backward. It should not be an issue on
         * systems where get_ostime() results in a true syscall.)
         */
        if (cmp_tspec(add_tspec_ns(ts, 50000000), ts_last) < 0)
                lamport_violated = true;
        ts_last = ts;

	/*
	 * After default_get_precision() has set a nonzero sys_fuzz,
	 * ensure every reading of the OS clock advances by at least
	 * sys_fuzz over the prior reading, thereby assuring each
	 * fuzzed result is strictly later than the prior.  Limit the
	 * necessary fiction to 1 second.
	 */
	ts_min = add_tspec_ns(ts_prev, sys_fuzz_nsec);
	if (cmp_tspec(ts, ts_min) < 0) {
		ts_lam = sub_tspec(ts_min, ts);
		if (ts_lam.tv_sec > 0 && !lamport_violated) {
			msyslog(LOG_ERR,
				"get_systime Lamport advance exceeds one second (%.9f)",
				ts_lam.tv_sec +
				    1e-9 * ts_lam.tv_nsec);
			exit(1);
		}
		if (!lamport_violated)
			ts = ts_min;
	}
	ts_prev_log = ts_prev;
	ts_prev = ts;

	/* convert from timespec to l_fp fixed-point */
	result = tspec_stamp_to_lfp(ts);

	/*
	 * Add in the fuzz.
	 */
	dfuzz = rand * 2. / FRAC * sys_fuzz;
	DTOLFP(dfuzz, &lfpfuzz);
	L_ADD(&result, &lfpfuzz);

	/*
	 * Ensure result is strictly greater than prior result (ignoring
	 * sys_residual's effect for now) once sys_fuzz has been
	 * determined.
	 */
	if (!L_ISZERO(&lfp_prev) && !lamport_violated) {
		if (!L_ISGTU(&result, &lfp_prev) &&
		    sys_fuzz > 0.) {
			msyslog(LOG_ERR, "ts_prev %s ts_min %s",
				tspectoa(ts_prev_log),
				tspectoa(ts_min));
			msyslog(LOG_ERR, "ts %s", tspectoa(ts));
			msyslog(LOG_ERR, "sys_fuzz %ld nsec, prior fuzz %.9f",
				sys_fuzz_nsec, dfuzz_prev);
			msyslog(LOG_ERR, "this fuzz %.9f",
				dfuzz);
			lfpdelta = lfp_prev;
			L_SUB(&lfpdelta, &result);
			LFPTOD(&lfpdelta, ddelta);
			msyslog(LOG_ERR,
				"prev get_systime 0x%x.%08x is %.9f later than 0x%x.%08x",
				lfp_prev.l_ui, lfp_prev.l_uf,
				ddelta, result.l_ui, result.l_uf);
		}
	}
	lfp_prev = result;
	dfuzz_prev = dfuzz;
	if (lamport_violated)
		lamport_violated = false;
	*now = result;
}


/*
 * adj_systime - adjust system time by the argument.
 */
bool				/* true on okay, false on error */
adj_systime(
	double now,		/* adjustment (s) */
	int (*ladjtime)(const struct timeval *, struct timeval *)
	)
{
	struct timeval adjtv;	/* new adjustment */
	struct timeval oadjtv;	/* residual adjustment */
	double	quant;		/* quantize to multiples of */
	double	dtemp;
	long	ticks;
	bool	isneg = false;

	/*
	 * FIXME: With the legacy Windows port gone, this might be removable.
	 * See also the related FIXME comment in ntpd/ntp_loopfilter.c.
	 *
	 * The Windows port adj_systime() depended on being called each
	 * second even when there's no additional correction, to allow
	 * emulation of adjtime() behavior on top of an API that simply
	 * sets the current rate.  This POSIX implementation needs to
	 * ignore invocations with zero correction, otherwise ongoing
	 * EVNT_NSET adjtime() can be aborted by a tiny adjtime()
	 * triggered by sys_residual.
	 */
	if (0. == now)
		return true;

	/*
	 * Most Unix adjtime() implementations adjust the system clock
	 * in microsecond quanta, but some adjust in 10-ms quanta. We
	 * carefully round the adjustment to the nearest quantum, then
	 * adjust in quanta and keep the residue for later.
	 */
	dtemp = now + sys_residual;
	if (dtemp < 0) {
		isneg = true;
		dtemp = -dtemp;
	}
	adjtv.tv_sec = (long)dtemp;
	dtemp -= adjtv.tv_sec;
	if (sys_tick > sys_fuzz)
		quant = sys_tick;
	else
		quant = 1e-6;
	ticks = (long)(dtemp / quant + .5);
	adjtv.tv_usec = (long)(ticks * quant * 1.e6 + .5);
	/* The rounding in the conversions could push us over the
	 * limits: make sure the result is properly normalised!
	 * note: sign comes later, all numbers non-negative here.
	 */
	if (adjtv.tv_usec >= 1000000) {
		adjtv.tv_sec  += 1;
		adjtv.tv_usec -= 1000000;
		dtemp         -= 1.;
	}
	/* set the new residual with leftover from correction */
	sys_residual = dtemp - adjtv.tv_usec * 1.e-6;

	/*
	 * Convert to signed seconds and microseconds for the Unix
	 * adjtime() system call. Note we purposely lose the adjtime()
	 * leftover.
	 */
	if (isneg) {
		adjtv.tv_sec = -adjtv.tv_sec;
		adjtv.tv_usec = -adjtv.tv_usec;
		sys_residual = -sys_residual;
	}
	if (adjtv.tv_sec != 0 || adjtv.tv_usec != 0) {
		if (ladjtime(&adjtv, &oadjtv) < 0) {
			msyslog(LOG_ERR, "adj_systime: %m");
			return false;
		}
	}
	return true;
}


/*
 * step_systime - step the system clock.
 */

bool
step_systime(
	double step,
	int (*settime)(struct timespec *)
	)
{
	time_t pivot; /* for ntp era unfolding */
	struct timespec timets, tslast, tsdiff;
	struct calendar jd;
	l_fp fp_ofs, fp_sys; /* offset and target system time in FP */

	/*
	 * Get pivot time for NTP era unfolding. Since we don't step
	 * very often, we can afford to do the whole calculation from
	 * scratch. And we're not in the time-critical path yet.
	 */
#if NTP_SIZEOF_TIME_T > 4
	/*
	 * This code makes sure the resulting time stamp for the new
	 * system time is in the 2^32 seconds starting at 1970-01-01,
	 * 00:00:00 UTC.
	 */
	pivot = 0x80000000;
#if USE_COMPILETIME_PIVOT
	/*
	 * Add the compile time minus 10 years to get a possible target
	 * area of (compile time - 10 years) to (compile time + 126
	 * years).  This should be sufficient for a given binary of
	 * NTPD.
	 */
	if (ntpcal_get_build_date(&jd)) {
		jd.year -= 10;
		pivot += ntpcal_date_to_time(&jd);
	} else {
		msyslog(LOG_ERR,
			"step_systime: assume 1970-01-01 as build date");
	}
#else
	UNUSED_LOCAL(jd);
#endif /* USE_COMPILETIME_PIVOT */
#else
	UNUSED_LOCAL(jd);
	/* This makes sure the resulting time stamp is on or after
	 * 1969-12-31/23:59:59 UTC and gives us additional two years,
	 * from the change of NTP era in 2036 to the UNIX rollover in
	 * 2038. (Minus one second, but that won't hurt.) We *really*
	 * need a longer 'time_t' after that!  Or a different baseline,
	 * but that would cause other serious trouble, too.
	 */
	pivot = 0x7FFFFFFF;
#endif

	/* get the complete jump distance as l_fp */
	DTOLFP(sys_residual, &fp_sys);
	DTOLFP(step,         &fp_ofs);
	L_ADD(&fp_ofs, &fp_sys);

	/* ---> time-critical path starts ---> */

	/* get the current time as l_fp (without fuzz) and as struct timespec */
	get_ostime(&timets);
	fp_sys = tspec_stamp_to_lfp(timets);

	/* only used for utmp/wtmpx time-step recording */
	tslast.tv_sec = timets.tv_sec;
	tslast.tv_nsec = timets.tv_nsec;

	/* get the target time as l_fp */
	L_ADD(&fp_sys, &fp_ofs);

	/* unfold the new system time */
	timets = lfp_stamp_to_tspec(fp_sys, &pivot);

	/* now set new system time */
	if (settime(&timets) != 0) {
		msyslog(LOG_ERR, "step_systime: %m");
		return false;
	}

	/* <--- time-critical path ended with 'ntp_set_tod()' <--- */

	sys_residual = 0;
	lamport_violated = (step < 0);
	if (step_callback)
		(*step_callback)();

	/*
	 * FreeBSD, for example, has:
	 * struct utmp {
	 *	   char    ut_line[UT_LINESIZE];
	 *	   char    ut_name[UT_NAMESIZE];
	 *	   char    ut_host[UT_HOSTSIZE];
	 *	   long    ut_time;
	 * };
	 * and appends line="|", name="date", host="", time for the OLD
	 * and appends line="{", name="date", host="", time for the NEW
	 * to _PATH_WTMP .
	 *
	 * Some OSes have utmp, some have utmpx. POSIX.1-2001 standardizes
	 * utmpx, so we'll support that.
	 */

	/*
	 * Write old and new time entries in utmp and wtmp if step
	 * adjustment is greater than one second.
	 *
	 * This might become even uglier...
	 */
	tsdiff = abs_tspec(sub_tspec(timets, tslast));
	if (tsdiff.tv_sec > 0) {
#ifdef HAVE_UTMPX_H
# ifdef OVERRIDE_OTIME_MSG
#  define OTIME_MSG OVERRIDE_OTIME_MSG
# else
/* Already defined on NetBSD */
#  ifndef OTIME_MSG
#   define OTIME_MSG	"Old NTP time"
#  endif
# endif
# ifdef OVERRIDE_NTIME_MSG
#  define NTIME_MSG OVERRIDE_NTIME_MSG
# else
#  ifndef NTIME_MSG
#   define NTIME_MSG	"New NTP time"
#  endif
# endif
		struct utmpx utx;

		ZERO(utx);

		/* UTMPX - this is POSIX-conformant */
		utx.ut_type = OLD_TIME;
		strlcpy(utx.ut_line, OTIME_MSG, sizeof(utx.ut_line));
		utx.ut_tv.tv_sec = tslast.tv_sec;
		utx.ut_tv.tv_usec = (tslast.tv_nsec + 500) / 1000;
		setutxent();
		pututxline(&utx);
		utx.ut_type = NEW_TIME;
		strlcpy(utx.ut_line, NTIME_MSG, sizeof(utx.ut_line));
		utx.ut_tv.tv_sec = timets.tv_sec;
		utx.ut_tv.tv_usec = (timets.tv_nsec + 500) / 1000;
		setutxent();
		pututxline(&utx);
		endutxent();

# undef OTIME_MSG
# undef NTIME_MSG
#endif
	}
	return true;
}
