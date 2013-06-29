CFLAGS=-Wall

multirec: clean
	gcc $(CFLAGS) -lasound -lncurses -lsamplerate -lsndfile -lpthread -o multirec multirec.c main.c worker.c buffer_queue.c

clean:
	rm -f multirec
