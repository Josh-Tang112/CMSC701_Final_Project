#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <getopt.h>

#define READ_SIZE 1024
int idx_chunk_size = 10000;
char *output_file = "output.idx";

typedef struct sequence_read {
    char identifier[READ_SIZE];
    char sequence[READ_SIZE];
    char plus[READ_SIZE];
    char quality[READ_SIZE];

} sequence_read_t;

//Prints the usage information on error
void print_usage(char *argv[]) {
    fprintf(stderr, "Usage: %s -c CHUNK_SIZE GZIP_FILE\n", argv[0]);
}

void print_help(char *argv[]) {
    fprintf(stderr, "index-builder builds an index into a gzipped FASTQ ");
    fprintf(stderr, "file to allow for parallel processing\n\n");
    fprintf(stderr, "Usage: %s -c CHUNK_SIZE GZIP_FILE\n", argv[0]);
    fprintf(stderr, "-c CHUNK_SIZE\tthe integer chunk size with which to ");
    fprintf(stderr, "store indexes into the gzip file (default 10000)\n"); 
    fprintf(stderr, "GZIP_FILE\t<gzip file> is a gzipped FASTQ file to index\n");
}

//Prints a sequence_read_t struct
void print_seq(sequence_read_t *seq) {
    printf("%s", seq->identifier);
    printf("%s", seq->sequence);
    printf("%s", seq->plus);
    printf("%s", seq->quality);
}

//req_seq reads a series of four lines into a sequence_read_t struct
void read_seq(gzFile file, sequence_read_t *seq) {
    char buffer[READ_SIZE];
    gzgets(file, buffer, READ_SIZE);
    strncpy(seq->identifier, buffer, strlen(buffer));
    gzgets(file, buffer, READ_SIZE);
    strncpy(seq->sequence, buffer, READ_SIZE);
    gzgets(file, buffer, READ_SIZE);
    strncpy(seq->plus, buffer, READ_SIZE);
    gzgets(file, buffer, READ_SIZE);
    strncpy(seq->quality, buffer, READ_SIZE);
}

int main(int argc, char *argv[]) {
    int opt;
    char *filename;

    while ((opt = getopt(argc, argv, "c:ho:")) != -1) {
        switch (opt) {
            case 'c': //chunk size 
                idx_chunk_size = atoi(optarg);
                break;
            case 'o': //output filename
                output_file = optarg;     
                break;
            case 'h':
                print_help(argv);
                return 0;
            default:
                print_usage(argv);
                return 1;
        }
    }

    if (optind >= argc) {
        print_usage(argv);
        return 1;
    }

    filename = argv[optind];

    gzFile file = gzopen(filename, "rb");
    if (!file) {
        fprintf(stderr, "Fatal error; failed to open %s\n", filename);
        return 1;
    }

    int n_sequences = 0;
    sequence_read_t seq;
    gz_header header;
    while (!gzeof(file)) {

        //Which number sequence is this that we've read?
        n_sequences++;
        //If this finishes a chunk of sequences, write out the lookup info
        if (n_sequences % idx_chunk_size == 0) {
        }

        //Read the sequence into a sequence_read_t
        read_seq(file, &seq);
        print_seq(&seq);



    }

    gzclose(file);
    return 0;
}

