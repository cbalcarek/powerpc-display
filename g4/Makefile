# Command-line ppcdisplay only, tuned for the Mac it's built on.
# For the app, double-click "Build PowerPC Display.command".
CC      = gcc
CODEC   = ../host/Sources/CCodec
CPU    := $(shell machine)
# (Tiger's make 3.80 has no "else ifeq")
ARCH    = $(if $(filter ppc750,$(CPU)),-mcpu=750 -mtune=750,\
          $(if $(filter ppc7400,$(CPU)),-mcpu=7400 -mtune=7400,\
          $(if $(filter ppc7450,$(CPU)),-mcpu=7450 -mtune=7450,\
          $(if $(filter ppc970,$(CPU)),-mcpu=970 -mtune=970,-mcpu=750 -mtune=7450))))
CFLAGS  = -O3 $(ARCH) -fomit-frame-pointer -std=gnu99 -Wall -I$(CODEC)/include
LIBS    = -framework OpenGL -framework ApplicationServices -framework Carbon

ppcdisplay: client.c $(CODEC)/g4codec.c $(CODEC)/include/g4codec.h
	$(CC) $(CFLAGS) -o $@ client.c $(CODEC)/g4codec.c $(LIBS)

clean:
	rm -f ppcdisplay

.PHONY: clean
