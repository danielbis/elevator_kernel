#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include "wrappers.h"

int main(int argc, char **argv) {
	if (argc != 2) {
		printf("wrong number of args\n");
		return -1;
	}
	
	if (strcmp(argv[1], "--start") == 0)
		start_elevator();
	else if (strcmp(argv[1], "--stop") == 0)
		stop_elevator();
	else
		return -1;
		
	return 0;
}
