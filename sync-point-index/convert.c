#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <zlib.h>
#include <getopt.h>
#include <time.h>
#include <stdint.h>
#include <pthread.h>

#define BUFSIZE 16384

int sequences_per_chunk = 10000;

void convert(const char* in_gz, const char* out_gz, const char* index_filename) {
    FILE *in, *out, *index;
    z_stream in_stream, out_stream;
    char in_buf[BUFSIZE];
    char d_out_buf[BUFSIZE]; /* output buffer for decompression */
    char c_out_buf[BUFSIZE]; /* output buffer for re-compression */
    uint64_t inflate_flush, deflate_flush;
    uint64_t line = 0, seq = 0;
    uint64_t next_sequence_start = 0, remaining_reads_len = 0;
    uint64_t totin = 0, totout = 0;
    uint64_t ret;

    if (!(in = fopen(in_gz, "rb"))) {
        printf("Could not open file %s for reading\n", in_gz);
        exit(1);
    }
    if (!(out = fopen(out_gz, "wb"))) {
        printf("Could not open file %s for writing\n", out_gz);
        exit(1);
    }
    if (!(index = fopen(index_filename, "wb"))) {
        printf("Could not open file %s for writing\n", index_filename);
        exit(1);
    }

    // Initialize stream for decompressing original gzip file
    in_stream.avail_in = 0;
    in_stream.avail_out = 0;
    in_stream.next_in = Z_NULL;
    in_stream.zalloc = Z_NULL;
    in_stream.zfree = Z_NULL;
    in_stream.opaque = Z_NULL;
    if (inflateInit2(&in_stream, 47) != Z_OK) {
        printf("Error initializing stream state for decompression\n");
	    exit(1);
    }

    // Initialize stream for recompressing gzip file with Z_FULL_FLUSH sync points
    out_stream.zalloc = Z_NULL;
    out_stream.zfree = Z_NULL;
    out_stream.opaque = Z_NULL;
    if (deflateInit2(&out_stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        printf("Error initializing stream state for recompression\n");
        exit(1);
    }
   
    do {
	    // Read in some data from original gzip
        in_stream.avail_in = fread(in_buf, 1, BUFSIZE, in);
	    if (ferror(in)) {
            ret = Z_ERRNO;
	        exit(1);
        }
        if (in_stream.avail_in == 0) {
            ret = Z_DATA_ERROR;
	        exit(1);
        }
        inflate_flush = feof(in) ? Z_FINISH : Z_SYNC_FLUSH;
        in_stream.next_in = (Bytef *) in_buf;

	    do {
	        in_stream.avail_out = BUFSIZE - remaining_reads_len;
            in_stream.next_out = (Bytef *) (d_out_buf + remaining_reads_len);
            remaining_reads_len= 0;

            ret = inflate(&in_stream, inflate_flush);
            uint64_t have = BUFSIZE - in_stream.avail_out;
            deflate_flush = (ret == Z_STREAM_END) ? Z_FINISH : Z_SYNC_FLUSH;

	        // Count number of complete sequences in the decompressed data
            for (uint64_t i = 0; i < have; i++) {
	            if (d_out_buf[i] == '\n') {
		            line++;
		            if ((line % 4) == 0) {
		                seq++;
                        if ((seq % sequences_per_chunk) == 0) {
                            deflate_flush = Z_FULL_FLUSH;
                            next_sequence_start = (i + 1) % BUFSIZE;
                            remaining_reads_len= have - next_sequence_start;
                            break;
                        }
                    }
		        }
            }

            out_stream.avail_in = have - remaining_reads_len;
            out_stream.next_in = (Bytef *) d_out_buf;
            do {
                out_stream.avail_out = BUFSIZE;
	            out_stream.next_out = (Bytef *) c_out_buf;

                totin += out_stream.avail_in;
		        totout += out_stream.avail_out;
                ret = deflate(&out_stream, deflate_flush);
                totin -= out_stream.avail_in;
		        totout -= out_stream.avail_out;

		        // Write the recompressed data (with sync points) to the output gzip file
	            fwrite(c_out_buf, 1, BUFSIZE - out_stream.avail_out, out);
            } while (out_stream.avail_out == 0);
            
            // Each index entry represented by a triplet:
            //     The byte offset of sync point in the gzip
            //     The byte offset in the uncompressed stream
            //     The number of the last read before the next sync point
            if (deflate_flush == Z_FULL_FLUSH || deflate_flush == Z_FINISH) {
		        // printf("totout: %d seq: %d\n", totout, seq);
	            fwrite(&totout, sizeof(uint64_t), 1, index);
                fwrite(&totin, sizeof(uint64_t), 1, index);
		        fwrite(&seq, sizeof(uint64_t), 1, index);
	        }
            
            // Move the bytes from the incomplete sequence to the start of the decompressed data output buffer
            // to free space for the next decompression step
            memmove(d_out_buf, d_out_buf + next_sequence_start, remaining_reads_len);
        } while (in_stream.avail_in != 0);
    } while(ret != Z_STREAM_END);

    fclose(in);
    fclose(out);
    fclose(index);
}

int main(int argc, char* argv[]) {
    if (!(argc == 4 || argc == 5)) {
        printf("Usage: %s <input gz> <output gz> <output idx> [reads per chunk]\n", argv[0]);
        exit(1);
    }
    if (argc == 5) {
        sequences_per_chunk = atoi(argv[4]);
    }

    time_t start_time = time(NULL);
    convert(argv[1],argv[2], argv[3]);
    time_t end_time = time(NULL);
    double elapsed_time = difftime(end_time, start_time);
    printf("Time: %f seconds\n", elapsed_time);
}