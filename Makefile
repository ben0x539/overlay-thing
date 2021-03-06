XCB_LIBS = xcb xcb-shape

all: overlay-thing

overlay-thing: main.o mumble.o xcb.o
	$(CC) $(CFLAGS) -o overlay-thing main.o mumble.o xcb.o $(LDFLAGS) -lrt `pkg-config --libs $(XCB_LIBS)`

main.o: main.c main.h xcb.h mumble.h overlay.h
	$(CC) $(CFLAGS) -c main.c

mumble.o: mumble.c main.h overlay.h mumble.h
	$(CC) $(CFLAGS) -c mumble.c

xcb.o: xcb.c main.h overlay.h xcb.h
	$(CC) $(CFLAGS) `pkg-config --cflags $(XCB_LIBS)` -c xcb.c

clean:
	rm -f mumble.o xcb.o main.o overlay-thing
