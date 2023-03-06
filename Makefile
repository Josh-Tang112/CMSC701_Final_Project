all: read-fastq

read-fastq: read-fastq.c
	gcc -o read-fastq read-fastq.c -lz
