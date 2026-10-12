# Round-trip + fuzz tests for the codec (any Unix with gcc/clang)
CODEC = ../host/Sources/CCodec

test: test_codec
	./test_codec

test_codec: test_codec.c $(CODEC)/g4codec.c $(CODEC)/include/g4codec.h
	$(CC) -O2 -g -std=c99 -Wall -Wextra -fsanitize=address,undefined -I$(CODEC)/include \
		-o $@ test_codec.c $(CODEC)/g4codec.c

clean:
	rm -f test_codec

.PHONY: test clean
