#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "wrappers.h"

int rnd(int min, int max) {
	return rand() % (max - min + 1) + min; //slight bias towards first k
}

int main(int argc, char **argv) {
	int times;
	int cnt;
	int i;
	int type;
	int start;
	int dest;
	long ret;
	
	srand(time(0));

	if (argc != 1) {
		printf("wrong number of args\n");
		return -1;
	}
	
	times = 50;
	cnt = 0;
	for (i = 0; i < times; i++) {
		type = rnd(1, 5); //off by 1
		start = rnd(1, 11); //off by 1
		dest = rnd(1, 11); //off by 1, can equal start
		
		ret = issue_request(type, start, dest);
		if (ret == 0)
			cnt++;
		
	}
	
	printf("Dispatched %d requests, %d of them were successful\n", times, cnt);

	return 0;
}
