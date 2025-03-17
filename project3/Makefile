CC = gcc
CFLAGS = -g -c -m32
AR = ar -rc
RANLIB = ranlib

my_vm.a: my_vm.o
	$(AR) $@ $^
	$(RANLIB) $@

clean:
	rm -rf *.o *.a
