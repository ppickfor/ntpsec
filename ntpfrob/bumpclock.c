/*
 * Hack to bump the system time for testing ntpd response.
 * Must be run as root.
 * Arg is microseconds.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NANOSECONDS	100000000

void bumpclock(int bump)
{
    struct timespec was, set, now;
    int rc1, rc2, rc3;
    int er1, er2, er3;

    printf("Bumping clock by %d microseconds.\n", bump);

    rc1 = clock_gettime(CLOCK_REALTIME, &was);
    er1 = errno;

    set = was;
    bump *= 1000;
    /* coverity[tainted_data] */
    set.tv_nsec += bump;
    while (set.tv_nsec >= NANOSECONDS ) {
	set.tv_nsec -= NANOSECONDS;
	set.tv_sec +=1;
    }
    while (set.tv_nsec <= 0 ) {
	set.tv_nsec += NANOSECONDS;
	set.tv_sec -=1;
    }
    rc2 = clock_settime(CLOCK_REALTIME, &set);
    er2 = errno;

    rc3 = clock_gettime(CLOCK_REALTIME, &now);
    er3 = errno;

    /* Defer printing so it doesn't distort timing. */
    if (rc1)
	printf("Couldn't get time: %s\n", strerror(er1));
    else
	printf("Was: %ld.%09ld\n", (long)was.tv_sec, was.tv_nsec);

    if (rc2) {
	printf("Couldn't set time: %s\n", strerror(er2));
	printf("Try: %ld.%09ld\n", (long)set.tv_sec, set.tv_nsec);
    } else
	printf("Set: %ld.%09ld\n", (long)set.tv_sec, set.tv_nsec);
 
   if (rc3)
	printf("Couldn't set time: %s\n", strerror(er3));
    else
	printf("Now: %ld.%09ld\n", (long)now.tv_sec, now.tv_nsec);
}
