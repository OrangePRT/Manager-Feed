all: clean manager feed

manager: manager.c
	gcc -o manager manager.c -lpthread 

feed: feed.c
	gcc -o feed feed.c -lpthread

clean:
	rm -f manager feed

broker:
	gcc -o manager manager.c -lpthread 

