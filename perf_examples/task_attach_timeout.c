/*
 * task_attach_timeout.c - attach to another task for monitoring for a short while
 *
 * Copyright (c) 2009 Google, Inc
 * Contributed by Stephane Eranian <eranian@gmail.com>
 *
 * Based on:
 * Copyright (c) 2002-2006 Hewlett-Packard Development Company, L.P.
 * Contributed by Stephane Eranian <eranian@hpl.hp.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
 * PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE
 * OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <sys/types.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h>
#include <sys/wait.h>
#include <err.h>
#include <sys/poll.h>

#include "perf_util.h"

typedef struct {
	char *events;
	int delay;
	int print;
	int group;
	int pinned;
} options_t;

static options_t options;

static void
print_counts(perf_event_desc_t *fds, int num, int do_delta)
{
	uint64_t values[3];
	ssize_t ret;
	int i;

	/*
	 * now simply read the results.
	 */
	for(i=0; i < num; i++) {
		uint64_t val;
		double ratio;

		ret = read(fds[i].fd, values, sizeof(values));
		if (ret < (ssize_t)sizeof(values)) {
			if (ret == -1)
				err(1, "cannot read values event %s", fds[i].name);
			else
				warnx("could not read event%d", i);
		}

		/*
		 * scaling because we may be sharing the PMU and
		 * thus may be multiplexed
		 */
		fds[i].prev_value = fds[i].value;
		fds[i].value = val = perf_scale(values);
		ratio = perf_scale_ratio(values);

		val = do_delta ? (val - fds[i].prev_value): val;

		if (ratio == 1.0)
			printf("%20"PRIu64" %s\n", val, fds[i].name);
		else
			if (ratio == 0.0)
				printf("%20"PRIu64" %s (did not run: incompatible events, too many events in a group, competing session)\n", val, fds[i].name);
			else
				printf("%20"PRIu64" %s (scaled from %.2f%% of time)\n", val, fds[i].name, ratio*100.0);

	}
}


int
measure(pid_t pid)
{
	perf_event_desc_t *fds = NULL;
	int i, ret, num_fds = 0;
	char fn[32];

	if (pfm_initialize() != PFM_SUCCESS)
		errx(1, "libpfm initialization failed\n");

	ret = perf_setup_list_events(options.events, &fds, &num_fds);
	if (ret || (num_fds == 0))
		exit(1);

	fds[0].fd = -1;
	for(i=0; i < num_fds; i++) {
		fds[i].hw.disabled = 0; /* start immediately */

		/* request timing information necessary for scaling counts */
		fds[i].hw.read_format = PERF_FORMAT_SCALE;
		fds[i].hw.pinned = !i && options.pinned;
		fds[i].fd = perf_event_open(&fds[i].hw, pid, -1, (options.group? fds[0].fd : -1), 0);
		if (fds[i].fd == -1)
			errx(1, "cannot attach event %s", fds[i].name);
	}
	/*
	 * no notification is generated by perf_counters
	 * when the monitored thread exits. Thus we need
	 * to poll /proc/ to detect it has disappeared,
	 * otherwise we have to wait until the end of the
	 * timeout
	 */
	sprintf(fn, "/proc/%d/status", pid);

	while(access(fn, F_OK) == 0 && options.delay) {
		sleep(1);
		options.delay--;
		if (options.print)
			print_counts(fds, num_fds, 1);
	}
	if (options.delay)
		warn("thread %d terminated before timeout", pid);

	if (!options.print)
		print_counts(fds, num_fds, 0);

	for(i=0; i < num_fds; i++)
		close(fds[i].fd);

	perf_free_fds(fds, num_fds);

	/* free libpfm resources cleanly */
	pfm_terminate();

	return 0;
}

static void
usage(void)
{
	printf("usage: task_attach_timeout [-h] [-p] [-P] [-g] [-d delay] [-e event1,event2,...] pid\n");
}

int
main(int argc, char **argv)
{
	int c;

	while ((c=getopt(argc, argv,"he:vd:pgP")) != -1) {
		switch(c) {
			case 'e':
				options.events = optarg;
				break;
			case 'p':
				options.print = 1;
				break;
			case 'P':
				options.pinned = 1;
				break;
			case 'g':
				options.group = 1;
				break;
			case 'd':
				options.delay = atoi(optarg);
				break;
			case 'h':
				usage();
				exit(0);
			default:
				errx(1, "unknown error");
		}
	}
	if (!options.events)
		options.events = strdup("PERF_COUNT_HW_CPU_CYCLES,PERF_COUNT_HW_INSTRUCTIONS");

	if (options.delay < 1)
		options.delay = 10;

	if (!argv[optind])
		errx(1, "you must specify pid to attach to\n");
	
	return measure(atoi(argv[optind]));
}
