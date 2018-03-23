#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "wrappers.h"

int rnd(int min, int max) {
	return rand() % (max - min + 1) + min; //slight bias towards first k
}

int main(int argc, char **argv) {
	srand(time(0));

	if (argc != 1) {
		printf("wrong number of args\n");
		return -1;
	}
		
	int type = rnd(0, 5); //off by 2
	int start = rnd(0, 11); //off by 2
	int dest = rnd(0, 11); //off by 2, can equal start
	
	long ret = issue_request(type, start, dest);
	printf("Issue (%d, %d, %d) returned %ld\n", type, start, dest, ret);

	return 0;
}
