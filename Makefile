CC = gcc
CFLAGS = -Wall -Wextra -std=c17 -O2
LDFLAGS = -lpthread
TARGET = planificador

all: $(TARGET)
	

$(TARGET): plan_dieciochero_so.c
	$(CC) $(CFLAGS) -o $(TARGET) plan_dieciochero_so.c

clean:
	rm -f $(TARGET)

.PHONY: all clean
