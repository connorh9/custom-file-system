BINS = hfs mkfs
CC = gcc
CFLAGS = -Wall -Werror -pedantic -std=gnu18 -g
FUSE_CFLAGS = `pkg-config fuse --cflags --libs`


.PHONY: all
all: $(BINS)

wfs:
	$(CC) $(CFLAGS) hfs.c $(FUSE_CFLAGS) -o hfs
mkfs:
	$(CC) $(CFLAGS) -o mkfs mkfs.c

.PHONY: clean
clean:
	rm -rf $(BINS)
