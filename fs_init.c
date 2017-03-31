#include <stdio.h>
#include <stdlib.h>
int main(int argc, char** argv) {
	if(argc != 3) {
		perror("Invalid number of arguments.\n");
		exit(1);
	}
	FILE *fp = fopen(argv[1], "w");
	int f_size = atoi(argv[2]) - 1;
	fseek(fp, f_size, SEEK_SET);
	fputc('\0', fp);
	fclose(fp);
}