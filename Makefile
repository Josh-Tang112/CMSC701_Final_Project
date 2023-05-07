all: index-builder index-reader 

index-builder: index-builder.c
	gcc -g -o index-builder index-builder.c -lz

index-reader: index-reader.c
	gcc -g -o index-reader index-reader.c -lz -lm

clean:
	rm index-reader index-builder
