#include <stdlib.h>
#include <stdio.h>


extern void* alloc(size_t size, size_t align);

int main(void)
{
	int* tab[10];

	for (int j = 0; j < 10; j++) {
		tab[j] = (int*) malloc(10*sizeof(int));

		for (int i = 0; i < 10; i++) {
			tab[j][i] = 3;
		}
	}

	for (int j = 0; j < 10; j++) {
		free(tab[j]);
	}

	alloc(10 * sizeof(int), 1);
}
