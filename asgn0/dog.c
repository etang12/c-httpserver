#define BUF_LEN 4096

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <err.h>


static void read_and_write(uint8_t *buffer, char args[]) {	//handles standard input, no files are given so no need to use open()
	while(1) {
		ssize_t bytes_read = read(STDIN_FILENO, buffer, sizeof(buffer));
		if(bytes_read < 0){
			warn("%s", args);
			break;
		}
		if(bytes_read == 0){
			break;
		}
		ssize_t bytes_written = write(STDOUT_FILENO, buffer, bytes_read);
		if(bytes_written < 0){
			warn("%s", args);
		}
	}
}

int main(int argc, char* argv[]) {	//handles standard input and opening files to read/write to standard output
	uint8_t buf[BUF_LEN];
	if(argc == 1) {
		read_and_write(buf, argv[0]);
		return 0;
	}
	for(int i = argc - 1; i > 0; i--) {
		if(!strcmp(argv[i], "-")) {			//decrement loop counter to handle file after "-"
			read_and_write(buf, argv[i]);
			i--;
		}
		if(i == 0) {
			return 0;
		}
		ssize_t fd = open(argv[i], O_RDONLY);
		while(1){
			if( fd < 0) {
				warn("%s", argv[i]);
				break;
			}
			ssize_t bytes_read = read(fd, buf, sizeof(buf));
			if(bytes_read < 0) {
				warn("%s", argv[i]);
				break;
			} else if(bytes_read == 0){
				break;
			}
			ssize_t bytes_written = write(STDOUT_FILENO, buf, bytes_read);
			if(bytes_written < 0){
				warn("%s",argv[i]);
			}
		}
		close(fd);
	}
	return 0;
}



