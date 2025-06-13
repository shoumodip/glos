CC = cc
CFLAGS = `cat compile_flags.txt` -g

QBEDIR = src/libqbe/lib
QBELIB = $(QBEDIR)/libqbe.a

HEADERS = $(wildcard src/*.h)
SOURCES = $(wildcard src/*.c)
OBJECTS = $(SOURCES:.c=.o)

glos: $(OBJECTS) $(QBELIB)
	cc -o $@ $(OBJECTS) -L$(QBEDIR) -lqbe

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

$(QBELIB):
	cd src/libqbe && make
