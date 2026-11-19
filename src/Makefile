CC = gcc
CFLAGS = -O3 -Wall -Wextra -pthread -mavx2 -mno-avx512f
TARGET = ../mlc_rebuilt

SRCS = mlc.c helper.c allocation.c intrd.c hittest.c combined-loadlat.c busythread.c fastdelay_dispatch.c adv_scan.c ddio.c c2c_bandwidth.c delay_pointer_chase.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) -lrt

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all clean
