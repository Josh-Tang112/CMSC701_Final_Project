#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <getopt.h>
#include <time.h>
#include <stdint.h>

#define WINDOW 32768U
#define CHUNK 16384         /* file input buffer size */

// WIP, needs cleanup

void convert(const char* in_gz, const char* out_gz, const char* index_filename) {
    FILE *in, *out, *index;
    z_stream in_stream, out_stream;
    char in_buf[CHUNK];
    char d_out_buf[CHUNK]; /* output buffer for decompression */
    char c_out_buf[CHUNK]; /* output buffer for re-compression */
    int flush, ret, ret2; /* TODO: rename ret2 */
    int totout = 0;
    int prev = 0; /* TODO: rename this to convey this is next record */
    int len = 0; /* TODO: length of all complete reads */

    int line = 0;
    int seq = 0;

    if (!(in = fopen(in_gz, "rb"))) {
        printf("Could not open file %s for reading\n", in_gz);
        return;
    }
    if (!(out = fopen(out_gz, "wb"))) {
        printf("Could not open file %s for writing\n", out_gz);
        return;
    }
    if (!(index = fopen(index_filename, "wb"))) {
        printf("Could not open file %s for writing\n", index_filename);
        return;
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
	    return;
    }

    // Initialize stream for recompressing gzip file with Z_FULL_FLUSH sync points
    out_stream.zalloc = Z_NULL;
    out_stream.zfree = Z_NULL;
    out_stream.opaque = Z_NULL;
    if (deflateInit2(&out_stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        printf("Error initializing stream state for recompression\n");
        return;
    }
   
    do {
	// Read in some data from original gzip
        in_stream.avail_in = fread(in_buf, 1, CHUNK, in);
	    if (ferror(in)) {
            ret = Z_ERRNO;
	        return;
        }
        if (in_stream.avail_in == 0) {
            ret = Z_DATA_ERROR;
	        return;
        }
        flush = feof(in) ? Z_FINISH : Z_SYNC_FLUSH;
        in_stream.next_in = in_buf;

	    do {
	        in_stream.avail_out = CHUNK - len;
            in_stream.next_out = d_out_buf + len;

            ret = inflate(&in_stream, flush);
            int have = CHUNK - in_stream.avail_out;

	        // Count number of complete sequences in the decompressed data
            for (int i = 0; i < have; i++) {
	            if (d_out_buf[i] == '\n') {
		            line++;
		            if ((line % 4) == 0) {
		                seq++;
		                prev = (i + 1) % CHUNK; // Start index of incomplete sequence
                    }
		        }
            }
	        len = have - prev; // Calculate length of only the complete sequences
	        line = seq * 4; // reset lines to last complete read

            out_stream.avail_in = have - len;
            out_stream.next_in = d_out_buf;
            // using Z_FULL_FLUSH every time means frequent sync points (which are an extra 4 bytes each
            // Should we have parameter to have sync points depend on amount of compressed bytes written instead
            // of number of decompressed bytes?
            ret2 = (ret == Z_STREAM_END) ? Z_FINISH : Z_FULL_FLUSH;
            do {
                out_stream.avail_out = CHUNK;
	            out_stream.next_out = c_out_buf;

		        totout += out_stream.avail_out;
                ret = deflate(&out_stream, ret2);
		        totout -= out_stream.avail_out;

		        // Write the recompressed data (with sync points) to the output gzip file
	            fwrite(c_out_buf, 1, CHUNK - out_stream.avail_out, out);
            } while (out_stream.avail_out == 0);
            
            // write out index here (totout, seq)
            // Each index entry represented by a pair:
            //     The byte offset of sync point in the gzip
            //     The number of the last read before the next sync point
            if (ret2 == Z_FULL_FLUSH) {
		        //printf("totout: %d seq: %d\n", totout, seq);
	            fwrite(&totout, sizeof(int), 1, index);
		        fwrite(&seq, sizeof(int), 1, index);
	        }
            
            // Move the bytes from the incomplete sequence to the start of the decompressed data output buffer
            // to free space for the next decompression step
            memmove(d_out_buf, d_out_buf + prev, len);
        } while (in_stream.avail_in != 0);
    } while(ret != Z_STREAM_END);

    fclose(in);
    fclose(out);

    return;
}

void read_from_checkpoint(const char* filename) {
	FILE* fp;
	z_stream stream;
	char compressed[CHUNK];
	char reads[CHUNK];
	int ret;
	int pos = 4250; // pos and num bytes to read should be determined from index

	if (!(fp = fopen(filename, "rb"))) {
	    printf("Cannot open file %s for reading\n", filename);
	    return;
	}

	fseek(fp, pos, SEEK_SET);
    stream.avail_in = fread(compressed, 1, 4089, fp);
    stream.avail_out = CHUNK;
    stream.next_in = compressed;
    stream.next_out = reads;
    stream.zalloc = Z_NULL;
    stream.zfree = Z_NULL;
    stream.opaque = Z_NULL;
    if (inflateInit2(&stream, (pos) ? -15 : 47) != Z_OK) {
        printf("Error initializing stream state for decompression\n");
        return;
    }

	ret = inflate(&stream, Z_FINISH);
	fwrite(reads, 1, CHUNK - stream.avail_out, stdout);
}

int main() {
	convert("test.fastq.gz", "out.fastq.gz", "index.idx");
	//read_from_checkpoint("out.fastq.gz");
	return 0;
}
