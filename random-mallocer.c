#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>

int allocs_count = 16384;

int alloc_min = 1024 * 1024;

int alloc_max = 1024 * 1024 * 128;

int iter_count = 0;

volatile uint64_t * cache_churn_area = NULL;
size_t cache_churn_size = 0;

char **alloc_pointers;


int main(int argc, char *argv[])
{
	int opt;
	while ((opt = getopt(argc, argv, "n:f:t:i:c:")) != -1) {
		switch (opt) {
		case 'n':
			allocs_count = atoi(optarg);
			break;
		case 'f':
			alloc_min = atoi(optarg);
			break;
		case 't':
			alloc_max = atoi(optarg);
			break;
                case 'c':
                        cache_churn_size = atoi(optarg);
                        cache_churn_area = (uint64_t*)malloc(cache_churn_size);
                        break;
		default:
			fprintf(stderr, "Usage: %s [-n <how many allocs>] [-f <smallest malloc>] [-t <largest malloc>] [-i <iterations>]\n",
				argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	if (!iter_count)
		iter_count = allocs_count * 1024;

	alloc_pointers = calloc(allocs_count, sizeof(char *));
	if (!alloc_pointers) {
		fprintf(stderr, "failed to allocate alloc_pointers\n");
		exit(EXIT_FAILURE);
	}

	for (;iter_count >= 0; iter_count--) {
		int idx = (int)(random() % allocs_count);
		int size;
		if (!(iter_count % 500000)) {
			printf("iter_count = %d\n", iter_count);
		}
		if (alloc_pointers[idx]) {
			free(alloc_pointers[idx]);
			alloc_pointers[idx] = NULL;
			continue;
		}
		size = (int)(random() % (alloc_max - alloc_min)) + alloc_min;
		alloc_pointers[idx] = malloc(size);
		if (!alloc_pointers[idx]) {
			fprintf(stderr, "failed to allocate %d. iter_count = %d\n",
				size, iter_count);
		}
                if (cache_churn_area) {
                        for (int i = 0; i < cache_churn_size / sizeof(*cache_churn_area); i++) {
                           cache_churn_area[i]++;
                        }
                        asm volatile ("" : : : "memory");
                }
	}

	return 0;
}
