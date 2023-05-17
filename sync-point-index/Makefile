all: convert read

convert:
	gcc convert.c -o convert -lz

read:
	gcc read.c -o read -lz -pthread

clean:
	rm -rf convert read
