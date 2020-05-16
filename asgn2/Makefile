CC = gcc
CFLAGS = -g -Wall -Wextra -Wpedantic -Wshadow -pthread
TARGET = httpserver

all: $(TARGET)

$(TARGET): $(TARGET).o
	$(CC) $(CFLAGS) -o $(TARGET) $(TARGET).o

$(TARGET).o: $(TARGET).c
	$(CC) $(CFLAGS) -c $(TARGET).c

clean:
	rm $(TARGET) $(TARGET).o
