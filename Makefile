all: overlay-thing

overlay-thing: main.o mumble.o xcb.o
	$(CC) $(LDFLAGS) $(CFLAGS) `pkg-config --libs xcb xcb-render` -lrt -o overlay-thing main.o mumble.o xcb.o

main.o: main.c main.h xcb.h mumble.h overlay.h
	$(CC) $(CFLAGS) -c main.c

mumble.o: mumble.c main.h overlay.h mumble.h
	$(CC) $(CFLAGS) -c mumble.c

xcb.o: xcb.c main.h overlay.h xcb.h
	$(CC) $(CFLAGS) `pkg-config --cflags xcb xcb-render` -c xcb.c

clean:
	rm -f mumble.o xcb.o main.o overlay-thing
