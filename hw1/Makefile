all: server hunter prey

server:
	gcc -g -o server server.c

hunter:
	gcc -o hunter -DHUNTER client.c

prey:
	gcc -o prey -DPREY client.c

test:
	./server < example.inp

dist:
	tar cvzf hw1.tar.gz Makefile *.c *.h

clean:
	rm -f server hunter prey smsgs smsgc

distclean: clean
	rm -f hw1.tar.gz
