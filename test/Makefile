# Round-trip + fuzz tests for the codec (any Unix with gcc/clang)
CODEC = ../host/Sources/CCodec

test: test_codec test_auth
	./test_auth
	./test_codec

test_codec: test_codec.c $(CODEC)/g4codec.c $(CODEC)/include/g4codec.h
	$(CC) -O2 -g -std=c99 -Wall -Wextra -fsanitize=address,undefined -I$(CODEC)/include \
		-o $@ test_codec.c $(CODEC)/g4codec.c

test_auth: test_auth.c $(CODEC)/g4auth.c $(CODEC)/g4nacl.c $(CODEC)/include/g4auth.h
	$(CC) -O1 -g -std=c99 -D_DEFAULT_SOURCE -Wall -fsanitize=address,undefined -I$(CODEC)/include \
		-o $@ test_auth.c $(CODEC)/g4auth.c $(CODEC)/g4nacl.c

clean:
	rm -f test_codec test_auth

.PHONY: test clean
