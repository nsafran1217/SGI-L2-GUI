#
# SGI-L2-GUI - Makefile for IRIX 6.5.x with MIPSpro C
#
#   make            build ./sgil2gui
#   make test       build + run the parser unit tests
#   make install    install binary + app-defaults (as root)
#   make clean
#
# For Linux / gcc test builds use:  make -f Makefile.linux
#

CC      = cc
ABI     = -n32 -mips3
OPT     = -O2
# 1174/1552: unused variables, 3201: unused parameter, 1209: constant controlling expr
WOFF    = -woff 1174,1209,1552,3201
CFLAGS  = $(ABI) $(OPT) -fullwarn $(WOFF)
LDFLAGS = $(ABI)
LIBS    = -lXm -lXt -lX11

PREFIX  = /usr/local
BINDIR  = $(PREFIX)/bin
APPDEF  = /usr/lib/X11/app-defaults

PROG    = sgil2gui

CORE_OBJS = src/util.o src/l2parse.o src/config.o src/l2proto.o src/l2net.o src/demo.o

OBJS    = $(CORE_OBJS) src/image.o src/skins.o src/rack.o src/l2client.o \
          src/dialogs.o src/main.o \
          src/img/img_o300.o src/img/img_o350.o src/img/img_tezro.o \
          src/img/img_a350_yellow.o src/img/img_a350_grey.o \
          src/img/img_a3000.o src/img/img_a3000ix.o src/img/img_nl4r.o \
          src/img/img_router.o src/img/img_router_altix.o

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(LDFLAGS) -o $(PROG) $(OBJS) $(LIBS)

.c.o:
	$(CC) $(CFLAGS) -c $< -o $@

# generated images are huge tables - no need to optimise them
src/img/img_o300.o src/img/img_o350.o src/img/img_tezro.o \
src/img/img_a350_yellow.o src/img/img_a350_grey.o src/img/img_a3000.o \
src/img/img_a3000ix.o src/img/img_nl4r.o src/img/img_router.o \
src/img/img_router_altix.o: src/image.h

test: tests/test_l2parse
	./tests/test_l2parse

tests/test_l2parse: tests/test_l2parse.c src/l2parse.o src/util.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o tests/test_l2parse tests/test_l2parse.c src/l2parse.o src/util.o

install: $(PROG)
	mkdir -p $(BINDIR)
	cp $(PROG) $(BINDIR)/$(PROG)
	chmod 755 $(BINDIR)/$(PROG)
	cp app-defaults/SgiL2Gui $(APPDEF)/SgiL2Gui
	chmod 644 $(APPDEF)/SgiL2Gui

clean:
	rm -f $(OBJS) $(PROG) tests/test_l2parse

# header dependencies
src/util.o: src/util.h
src/l2parse.o: src/l2parse.h src/util.h
src/config.o: src/config.h src/l2parse.h src/util.h
src/l2proto.o: src/l2proto.h src/util.h
src/l2net.o: src/l2net.h src/l2proto.h src/util.h
src/demo.o: src/demo.h src/util.h
src/image.o: src/image.h src/util.h
src/skins.o: src/skins.h src/image.h src/l2parse.h src/img/images.h
src/rack.o: src/rack.h src/skins.h src/image.h src/l2parse.h src/util.h
src/l2client.o: src/l2client.h src/config.h src/l2net.h src/l2proto.h src/demo.h \
                src/util.h
src/dialogs.o: src/app.h src/config.h src/l2parse.h src/l2client.h src/rack.h src/util.h
src/main.o: src/app.h src/config.h src/l2parse.h src/l2client.h src/rack.h \
            src/l2net.h src/demo.h src/util.h
