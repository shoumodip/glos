CC = cc
CFLAGS = `cat compile_flags.txt` -g

LIBQBE  = src/libqbe/lib/libqbe.a
HEADERS = $(wildcard src/*.h)
SOURCES = $(wildcard src/*.c)
OBJECTS = $(SOURCES:.c=.o)

glos: $(OBJECTS) $(LIBQBE)
	cc -o $@ $(OBJECTS)

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIBQBE):
	cd src/libqbe && make
