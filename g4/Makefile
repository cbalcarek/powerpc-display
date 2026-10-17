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

SRCS    = client.c $(CODEC)/g4codec.c $(CODEC)/g4auth.c $(CODEC)/g4nacl.c

# plain C (no AltiVec): Tiger's gcc 4.0 can run out of memory on the AltiVec
# file, so "Build PowerPC Display.command" tries that part separately
ppcdisplay: $(SRCS) $(CODEC)/include/g4codec.h
	$(CC) $(CFLAGS) -mno-altivec -o $@ $(SRCS) $(LIBS)

clean:
	rm -f ppcdisplay

.PHONY: clean
