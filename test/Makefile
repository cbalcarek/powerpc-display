# Round-trip + fuzz tests for the codec (any Unix with gcc/clang)
CODEC = ../host/Sources/CCodec

test: test_codec test_auth test_yuv_simd
	./test_auth
	./test_yuv_simd
	./test_codec

test_codec: test_codec.c $(CODEC)/g4codec.c $(CODEC)/include/g4codec.h
	$(CC) -O2 -g -std=c99 -Wall -Wextra -fsanitize=address,undefined -I$(CODEC)/include \
		-o $@ test_codec.c $(CODEC)/g4codec.c

test_auth: test_auth.c $(CODEC)/g4auth.c $(CODEC)/g4nacl.c $(CODEC)/include/g4auth.h
	$(CC) -O1 -g -std=c99 -D_DEFAULT_SOURCE -Wall -fsanitize=address,undefined -I$(CODEC)/include \
		-o $@ test_auth.c $(CODEC)/g4auth.c $(CODEC)/g4nacl.c

test_yuv_simd: test_yuv_simd.c $(CODEC)/g4codec.c
	$(CC) -O2 -std=c99 -I$(CODEC)/include -o $@ test_yuv_simd.c $(CODEC)/g4codec.c

# the AltiVec path, on an emulated G4 (needs powerpc-linux-gnu-gcc + qemu-ppc)
test-altivec:
	powerpc-linux-gnu-gcc -O3 -std=c99 -static -mcpu=7450 -maltivec -mabi=altivec -I$(CODEC)/include \
		-o test_yuv_simd_ppc test_yuv_simd.c $(CODEC)/g4codec.c && qemu-ppc -cpu 7450 ./test_yuv_simd_ppc

clean:
	rm -f test_codec test_auth test_yuv_simd test_yuv_simd_ppc

.PHONY: test clean
