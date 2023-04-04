all: index-builder index-reader

index-builder: index-builder.c
	gcc -o index-builder index-builder.c -lz

index-reader: index-reader.c
	gcc -o index-reader index-reader.c -lz
