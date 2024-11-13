CFLAGS= -Wall -Wextra -Werror -Wno-unused-function -ggdb

prog = bsh
objects = bsh.o mu.o
headers = mu.h list.h

$(prog): $(objects)
	$(CC) -o $@ $^

$(objects) : %.o : %.c $(headers)
	$(CC) -o $@ -c $(CFLAGS) $<

clean:
	rm -rf $(prog) $(objects)
