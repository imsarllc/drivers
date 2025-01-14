all: imdma-example imdma-perf imdma-dump imdma-ioctls

imdma-example: imdma-example.c libimdma.o
	$(CC) -g -o imdma-example imdma-example.c libimdma.o

imdma-perf: imdma-perf.cpp libimdma.o
	$(CXX) -g -o imdma-perf imdma-perf.cpp libimdma.o

imdma-dump: imdma-dump.cpp libimdma.o
	$(CXX) -g -o imdma-dump imdma-dump.cpp libimdma.o

imdma-ioctls: imdma-ioctls.c
	$(CXX) -g -o imdma-ioctls imdma-ioctls.c

libimdma.o: libimdma.c libimdma.h
	$(CC) -g -I../imdma -o libimdma.o -c libimdma.c

clean:
	rm -f libimdma.o imdma-example imdma-perf imdma-dump imdma-ioctls
