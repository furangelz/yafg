.POSIX:

CC = c99
CFLAGS = -O1
LDFLAGS = -lz -lcrypto

SRC = git.c main.c
OBJ = $(SRC:.c=.o)

all: yafg

yafg: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

.c.o:
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f yafg git.o main.o $(OBJ)

install: all
	install -m 755 yafg /usr/local/bin/yafg

uninstall:
	rm -f /usr/local/bin/yafg
