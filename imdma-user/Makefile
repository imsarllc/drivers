all: imdma-example imdma-perf

imdma-example: imdma-example.c libimdma.o
	$(CC) -g -o imdma-example imdma-example.c libimdma.o

imdma-perf: imdma-perf.cpp libimdma.o
	$(CXX) -g -o imdma-perf imdma-perf.cpp libimdma.o

libimdma.o: libimdma.c
	$(CC) -g -I../imdma -o libimdma.o -c libimdma.c

clean:
	rm -f libimdma.o imdma-example imdma-perf
