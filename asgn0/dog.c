//#include "dog.h"
#define BUF_LEN 256

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <err.h>


static void write_to(ssize_t b, uint8_t *buffer) {
	while((b = read(STDIN_FILENO, buffer, sizeof(buffer)) > 0)) {
		b = write(STDOUT_FILENO, buffer, b);
	}
}

int main(int argc, char* argv[]) {

	uint8_t buf[BUF_LEN];
	ssize_t bytes = 0;

	if(argc == 1) {
		write_to(bytes, buf);
		return 0;
	}
	for(int i = argc - 1; i > 0; i--) {
		if(!strcmp(argv[i], "-")) {
			write_to(bytes, buf);
			i--;
		}
		if(i == 0) {
			return 0;
		}
		uint8_t fd = open(argv[i], O_RDONLY);
		if( fd < 0) {
			warn("%s", argv[i]);
		}
		while((bytes = read(fd, buf, sizeof(buf))) > 0) {
			if(bytes < 0) {
				warn("%s", argv[i]);
				break;
			}
			bytes = write(STDOUT_FILENO, buf, bytes);
		}
		close(fd);
	}
}


