linenoise_example: linenoise.h linenoise.c

linenoise_example: linenoise.o example.o
	$(CC) -Wall -W -Os -g -o linenoise_example linenoise.o example.o

.c.o:
	$(CC) -c -Wall -W -Os -g $<

clean:
	rm -f linenoise_example
