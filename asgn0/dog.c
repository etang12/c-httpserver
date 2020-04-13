//#include "dog.h"
#define BUF_LEN 256

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <err.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>


int main(int argc, char* argv[]) {

	uint8_t buf[BUF_LEN];
	ssize_t bytes;

	if(argc == 1 || !strcmp(argv[1], "-")) {
		while((bytes = read(STDIN_FILENO, buf, sizeof(buf))) != 0) {
			write(STDIN_FILENO, buf, sizeof(buf));
		}
		return 0;
	}
	for(int i = argc - 1; i > 0; i--) {
		uint8_t fd = open(argv[i], O_RDONLY);
		if( fd < 0) {
			warn("%s", argv[i]);
		}
		while(1) {
			bytes = read(fd, buf, sizeof(buf));
			if(bytes < 0) {
				warn("%s", argv[i]);
				break;
			}
			if(bytes == 0) {
				break;
			}
			//bytes = read(fd, buf, sizeof(buf));
			bytes = write(STDOUT_FILENO, buf, bytes);
		}
		close(fd);
	}
}