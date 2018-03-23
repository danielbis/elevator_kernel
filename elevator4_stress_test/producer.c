#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include "wrappers.h"

int time_set(struct timeval *t, int sec) {
	t->tv_sec = sec;
	t->tv_usec = 0;
	return 0;
}

/* Source: http://www.gnu.org/software/libc/manual/html_node/Elapsed-Time.html */
int time_diff(struct timeval *result, struct timeval *x, struct timeval *y) {
	int nsec;
	
	/* Perform the carry for the later subtraction by updating y. */
	if (x->tv_usec < y->tv_usec) {
		nsec = (y->tv_usec - x->tv_usec) / 1000000 + 1;
		y->tv_usec -= 1000000 * nsec;
		y->tv_sec += nsec;
	}
	if (x->tv_usec - y->tv_usec > 1000000) {
		nsec = (x->tv_usec - y->tv_usec) / 1000000;
		y->tv_usec += 1000000 * nsec;
		y->tv_sec -= nsec;
	}

	/* Compute the time remaining to wait. tv_usec is certainly positive. */
	result->tv_sec = x->tv_sec - y->tv_sec;
	result->tv_usec = x->tv_usec - y->tv_usec;

	/* Return 1 if result is negative. */
	return x->tv_sec < y->tv_sec;
}

int time_sleep(struct timeval *t) {
	printf("s %ld\n", t->tv_sec);
	printf("us %ld\n", t->tv_usec);
	sleep(t->tv_sec);
	usleep(t->tv_usec);
	return 0;
}

/******************************************************************************/

int rnd(int min, int max) {
	return rand() % (max - min + 1) + min; //slight bias towards first k
}

int rnd_dest(int start) {
	int chance;
	int ret;
	
	chance = rnd(0, 100);
	
	//70%-ish chance of choose 1 as a destination (if not already on floor 1)
	if (chance <= 70 && start != 1)
		ret = 1;
	else {
		do {
			ret = rnd(2, 10);
		} while (ret == start);
	}

	return ret;
}

/******************************************************************************/

int main(int argc, char **argv) {
	const int times = 1000000; 		// 1 million
	const int total_time = 5 * 60; 	// 5 mins
	
	int i;
	int type;
	int start;
	int dest;

	struct timeval t1;
	struct timeval t2;
	struct timeval elapsed;
	struct timeval total;
	struct timeval sleep;
	
	if (argc != 1) {
		printf("wrong number of args\n");
		return -1;
	}
	
	srand(17); //fixed to ensure everyone gets the same set of requests
	time_set(&total, total_time);
	
	gettimeofday(&t1, NULL);	
	for (i = 0; i < times; i++) {
		type = rnd(1, 4); 
		start = rnd(1, 10); 
		dest = rnd_dest(start); 
		issue_request(type, start, dest);
	}
	gettimeofday(&t2, NULL);
	
	time_diff(&elapsed, &t2, &t1);
	if (time_diff(&sleep, &total, &elapsed) == 0)
		time_sleep(&sleep);

	return 0;
}
