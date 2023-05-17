all: index-builder index-reader base-counter

index-builder: index-builder.c
	gcc -g -o index-builder index-builder.c -lz

index-reader: index-reader.c
	gcc -g -o index-reader index-reader.c -lz -lm

base-counter: base-counter.c
	gcc -g -o base-counter base-counter.c -lz -lm

clean:
	rm index-reader index-builder base-counter
