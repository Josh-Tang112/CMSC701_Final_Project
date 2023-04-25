#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <getopt.h>
#include <time.h>
#include <stdint.h>
#include <errno.h>

#define WINSIZE 32768U          /* sliding window size */
#define CHUNKSIZE 16384         /* file input buffer size */
#define MAXLINE 2 * WINSIZE

struct seq_entry {
    int seq_num;       /* Sequence number */
    off_t start;       /* Offset from the start of block */
    int block;         /* Block number this sequence starts in */
};

struct seq_list {
    int have;           /* Number of seq_entries */
    int size;           /* Number of seq_entries we can have */
    void *seq_entry;    /* List of seq_entries */
};

int base64_decode(char* input, size_t input_len, char* output, size_t output_len) {
    int i, j;
    unsigned char c;
    unsigned char buffer[4];
    unsigned char temp[3];

    for (i = 0, j = 0; i < input_len; i += 4, j += 3) {
        // Extract 4 base64-encoded characters from input
        buffer[0] = input[i];
        buffer[1] = input[i+1];
        buffer[2] = input[i+2];
        buffer[3] = input[i+3];

        // Convert base64-encoded characters to their 6-bit values
        for (int k = 0; k < 4; k++) {
            if (buffer[k] >= 'A' && buffer[k] <= 'Z') {
                buffer[k] -= 'A';
            } else if (buffer[k] >= 'a' && buffer[k] <= 'z') {
                buffer[k] -= 'a' - 26;
            } else if (buffer[k] >= '0' && buffer[k] <= '9') {
                buffer[k] -= '0' - 52;
            } else if (buffer[k] == '+') {
                buffer[k] = 62;
            } else if (buffer[k] == '/') {
                buffer[k] = 63;
            }
        }

        // Combine 4 6-bit values into 3 bytes
        temp[0] = (buffer[0] << 2) | (buffer[1] >> 4);
        temp[1] = (buffer[1] << 4) | (buffer[2] >> 2);
        temp[2] = (buffer[2] << 6) | buffer[3];

        // Copy the decoded bytes to output
        if (j + 2 < output_len) {
            output[j] = temp[0];
            output[j+1] = temp[1];
            output[j+2] = temp[2];
        } else {
            return -1; // Output buffer too small
        }
    }

    // Add null terminator to output string
    output[j] = '\0';

    return j; // Return the number of bytes written to output
}

/* access point entry  FROM ZRAN */
struct point {
    off_t out;          /* corresponding offset in uncompressed data */
    off_t in;           /* offset in input file of first full byte */
    int bits;           /* number of bits (1-7) from byte at in - 1, or 0 */
    unsigned char window[WINSIZE];  /* preceding 32K of uncompressed data */
};

/* access point list */
struct access {
    int have;           /* number of list entries filled in */
    int size;           /* number of list entries allocated */
    struct point *list; /* allocated list */
};

/* Deallocate an index built by build_index() */
static void free_index(struct access *index)
{
    if (index != NULL) {
        free(index->list);
        free(index);
    }
}
/* From ZRAN */

static struct seq_list * add_seq(struct seq_list * list, int seqNum,
                                 off_t start, int blockNum) {

    /* Next sequence entry in the list */
    struct seq_entry *next;

    if (NULL == list) {
        list = malloc(sizeof(struct seq_list));
        if (NULL == list) return NULL;
        list->seq_entry= malloc(sizeof(struct seq_entry) << 3);
        if (list->seq_entry== NULL) {
            free(list);
            return NULL;
        }
        list->size = 8;
        list->have = 0;
    }
        /* if list is full, make it bigger */
    else if (list->have == list->size) {
        list->size <<= 1;
        next = realloc(list->seq_entry, sizeof(struct seq_entry) * list->size);
        if (next == NULL) {
            /* TODO free stuff */
            //deflate_index_free(index);
            return NULL;
        }
        list->seq_entry = next;
    }

    /* fill in entry and increment how many we have */
    next = (struct seq_entry *)(list->seq_entry) + list->have;
    next->seq_num = seqNum;
    next->start = start;
    next->block = blockNum;
    list->have++;

    /* return list, possibly reallocated */
    return list;
}

static struct access *add_read_point(struct access *index, int bits,
                              off_t in, off_t out, unsigned char *window)
{
    struct point *next;

    /* if list is empty, create it (start with eight points) */
    if (index == NULL) {
        index = malloc(sizeof(struct access));
        if (index == NULL) return NULL;
        index->list = malloc(sizeof(struct point) << 3);
        if (index->list == NULL) {
            free(index);
            return NULL;
        }
        index->size = 8;
        index->have = 0;
    }

        /* if list is full, make it bigger */
    else if (index->have == index->size) {
        index->size <<= 1;
        next = realloc(index->list, sizeof(struct point) * index->size);
        if (next == NULL) {
            free_index(index);
            return NULL;
        }
        index->list = next;
    }

    /* fill in entry and increment how many we have */
    next = index->list + index->have;
    next->bits = bits;
    next->in = in;
    next->out = out;
    strcpy(next->window, window);
    index->have++;

    /* return list, possibly reallocated */
    return index;
}

static int extract(FILE *in, struct access *index, struct point * this,
        off_t seq_offset, unsigned char *buf, int chunksize)
{
    int ret, skip, seq_num, out_idx;
    z_stream strm;
    unsigned char input[CHUNKSIZE];
    unsigned char discard[WINSIZE];
    int len;
    int totout = seq_num = out_idx = 0;
    int line_num = skip = 1;
    int buffsize = 2 * WINSIZE;
    char * output = (char *) malloc(buffsize * sizeof(char));

    /* initialize file and inflate state to start there */
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    strm.avail_in = 0;
    strm.next_in = Z_NULL;
    ret = inflateInit2(&strm, -15);         /* raw inflate */
    if (ret != Z_OK)
        return ret;

    printf("This->in: %lu\n", this->in);
    printf("Offset: %lu\n", this->in - (this->bits ? 1 : 0));
    off_t seek_offset = this->in - (off_t) (this->bits ? 1 : 0);
    printf("Offset: %lu\n", seek_offset);

    ret = fseeko(in, seek_offset, SEEK_SET);
    if (ret == -1) {
        perror("failed");
        printf("Error seeking to offset: %s\n", strerror(errno));
        goto deflate_index_extract_ret;
    }
    if (this->bits) {
        ret = getc(in);
        if (ret == -1) {
            ret = ferror(in) ? Z_ERRNO : Z_DATA_ERROR;
            goto deflate_index_extract_ret;
        }
        (void)inflatePrime(&strm, this->bits, ret >> (8 - this->bits));
    }
    (void)inflateSetDictionary(&strm, this->window, WINSIZE);

    /* skip uncompressed bytes until offset reached, then satisfy request */
    printf("seq_offset: %lu\n", seq_offset);
    seq_offset -= this->out;
    strm.avail_in = 0;
    do {
        /* define where to put uncompressed data, and how much */
        if (seq_offset > WINSIZE) {             /* skip WINSIZE bytes */
            printf("Skipping WINSIZE bytes\n");
            strm.avail_out = WINSIZE;
            strm.next_out = discard;
            seq_offset -= WINSIZE;

/*
            for (int i = 0; i < strm.avail_out; i++) {
                printf("%c", discard[i]);
            }
*/
        }
        else if (seq_offset > 0) {              /* last skip */
            strm.avail_out = (unsigned)seq_offset;
            strm.next_out = discard;
            seq_offset = 0;
/*
            for (int i = 0; i < strm.avail_out; i++) {
                printf("%c", discard[i]);
            }
*/
        }
        else if (skip) {                    /* at offset now */
            strm.avail_out = WINSIZE;
            strm.next_out = buf;
            skip = 0;                       /* only do this once */
        } else if (skip == 0) {
            strm.avail_out = WINSIZE;
            strm.next_out = buf;
            if (totout) {
                for (int i = 0; i < strm.avail_out; i++) {
                    //Copy the character into the output buffer
                    output[out_idx] = strm.next_out[i];
                    out_idx++; //Move index to next pos

                    //Check if it's a new line
                    if (strm.next_out[i] == '\n') {
                        // This is a new sequence
                        if ((line_num % 4) == 0) {
                            seq_num++;
                            /* If we've seen the total number of sequences we need to,
                             * return the buffer we've been building */
                            if ((seq_num % chunksize) == 0) {
                                printf("Stopping at seqnum %d\n", seq_num);
                                printf("Buf: %s\n", output);
                                exit(12);
                                /* This position is (totout - strm.avail_out) + i */
                            }
                        }
                    line_num++;
                    }

                   //Check if we need to double the output buffer size
                   if (out_idx == buffsize) {
                       printf("Reallocating, buffsize: %d\n", buffsize);
                       buffsize *= 2;
                       output = (char*) realloc(output, buffsize * sizeof(char));
                   }

                }
            }
        }

        //skip = 0;                       /* only do this once */
        /* uncompress until avail_out filled, or end of stream */
        do {
            if (strm.avail_in == 0) {
                strm.avail_in = fread(input, 1, CHUNKSIZE, in);
                if (ferror(in)) {
                    ret = Z_ERRNO;
                    goto deflate_index_extract_ret;
                }
                if (strm.avail_in == 0) {
                    ret = Z_DATA_ERROR;
                    goto deflate_index_extract_ret;
                }
                strm.next_in = input;
            }
            totout += strm.avail_out;
            ret = inflate(&strm, Z_SYNC_FLUSH);       /* normal inflate */
            totout -= strm.avail_out;
            if (ret == Z_NEED_DICT)
                ret = Z_DATA_ERROR;
            if (ret == Z_MEM_ERROR || ret == Z_DATA_ERROR)
                goto deflate_index_extract_ret;
            if (ret == Z_STREAM_END) {
                /* the raw deflate stream has ended */
                if (index->have == 0) {
                    /* this is a zlib stream that has ended -- done */
                    printf("Doing this break\n");
                    break;
                }

                /* near the end of a gzip member, which might be followed by
                   another gzip member -- skip the gzip trailer and see if
                   there is more input after it */
                if (strm.avail_in < 8) {
                    fseeko(in, 8 - strm.avail_in, SEEK_CUR);
                    strm.avail_in = 0;
                }
                else {
                    strm.avail_in -= 8;
                    strm.next_in += 8;
                }
                if (strm.avail_in == 0 && ungetc(getc(in), in) == EOF) {
                    /* the input ended after the gzip trailer -- done */
                    printf("Doing this break2\n");
                    break;
                }

                /* there is more input, so another gzip member should follow --
                   validate and skip the gzip header */
                ret = inflateReset2(&strm, 31);
                if (ret != Z_OK)
                    goto deflate_index_extract_ret;
                do {
                    if (strm.avail_in == 0) {
                        strm.avail_in = fread(input, 1, CHUNKSIZE, in);
                        if (ferror(in)) {
                            ret = Z_ERRNO;
                            goto deflate_index_extract_ret;
                        }
                        if (strm.avail_in == 0) {
                            ret = Z_DATA_ERROR;
                            goto deflate_index_extract_ret;
                        }
                        strm.next_in = input;
                    }

                    totout += strm.avail_out;
                    ret = inflate(&strm, Z_BLOCK);
                    totout -= strm.avail_out;
                    if (ret == Z_MEM_ERROR || ret == Z_DATA_ERROR)
                        goto deflate_index_extract_ret;
                } while ((strm.data_type & 128) == 0);

                /* set up to continue decompression of the raw deflate stream
                   that follows the gzip header */
                ret = inflateReset2(&strm, -15);
                if (ret != Z_OK)
                    goto deflate_index_extract_ret;
            }

            /* continue to process the available input before reading more */
        } while (strm.avail_out != 0);

        if (ret == Z_STREAM_END) {
            /* reached the end of the compressed data -- return the data that
               was available, possibly less than requested */
            printf("Reached the stream end\n");
            printf("avail-out: %d\n", strm.avail_out);
            //strm.avail_out = WINSIZE;
            strm.next_out = buf;
            //skip = 0;                       /* only do this once */
            if (totout) {
                for (int i = 0; i < WINSIZE - strm.avail_out; i++) {
                    output[out_idx] = strm.next_out[i];
                    out_idx++; //Move index to next pos

                    //Check if it's a new line
                    if (strm.next_out[i] == '\n') {
                        // This is a new sequence
                        if ((line_num % 4) == 0) {
                            seq_num++;
                            /* If we've seen the total number of sequences we need to,
                             * return the buffer we've been building */
                            if ((seq_num % chunksize) == 0) {
                                printf("Stopping at seqnum %d\n", seq_num);
                                printf("Buf: %s\n", output);
                                exit(12);
                                /* This position is (totout - strm.avail_out) + i */
                            }
                        }
                        line_num++;
                    }

                    //Check if we need to double the output buffer size
                    if (out_idx == buffsize) {
                        printf("Reallocating, buffsize: %d\n", buffsize);
                        buffsize *= 2;
                        output = (char*) realloc(output, buffsize * sizeof(char));
                    }
                }
            }
            break;
        }

        /* do until offset reached and requested data read */
    } while (1);

    printf("Got here after the while\n");

    /* compute the number of uncompressed bytes read after the offset */
    ret = skip ? 0 : len - strm.avail_out;

    /* clean up and return the bytes read, or the negative error */
    deflate_index_extract_ret:
/*
    for (int i = 0; i < strm.avail_out; i++) {
        printf("%c", strm.next_out[i]);
    }
*/

    (void)inflateEnd(&strm);


    return ret;


}

int main(int argc, char *argv[]) {
    FILE* fp;
    char line[MAXLINE];
    char* token;
    struct access * index;
    struct seq_list * list;
    struct point pt;
    struct seq_entry se;
    unsigned char buf[CHUNKSIZE];

    if (argc != 4) {
        fprintf(stderr, "usage: %s index.csv seq-index.csv file.gz\n", argv[0]);
        return -1;
    }

    // Open the CSV file for reading
    fp = fopen(argv[1], "r");
    if (fp == NULL) {
        printf("Error opening the file.\n");
        exit(1);
    }

    // Read each line of the file
    while (fgets(line, MAXLINE, fp) != NULL) {

        int decoded_len;
        char * decoded_string;

        /* Ignore comments */
        if (line[0] == '#') {
            continue;
        }

        /* Start sequence number */
        token = strtok(line, ",");


        token = strtok(NULL, ",");
        pt.out = atol(token);

        token = strtok(NULL, ",");
        pt.in = atol(token);

        token = strtok(NULL, ",");
        pt.bits = atoi(token);

        token = strtok(NULL, ",");


        size_t encoded_len = strlen(token);

        // Calculate the maximum length of the decoded string
        size_t max_decoded_len = (encoded_len * 3);
        decoded_string = (char*)malloc(max_decoded_len + 1);

        if (decoded_string == NULL) {
            printf("Error: Memory allocation failed.\n");
            return 1;
        }

        decoded_len = base64_decode(token, encoded_len, decoded_string, max_decoded_len);
        if ((decoded_len) < 0) {
            fprintf(stderr, "Error: Decoded_len < 0\n");
            exit(129);
        }
        decoded_string[WINSIZE] = '\0';
        strcpy(pt.window, decoded_string);

        index = add_read_point(index, pt.bits, pt.in, pt.out, decoded_string);
    }

    // Close the file
    fclose(fp);

    // Open the sequence CSV file for reading
    fp = fopen(argv[2], "r");
    if (fp == NULL) {
        printf("Error opening the sequence-index file.\n");
        exit(1);
    }

    // Read each line of the file
    while (fgets(line, MAXLINE, fp) != NULL) {
        /* Ignore comments */
        if (line[0] == '#') {
            continue;
        }

        /* Start sequence number */
        token = strtok(line, ",");
        se.seq_num = atol(token);

        token = strtok(NULL, ",");
        se.block = atoi(token);

        token = strtok(NULL, ",");
        se.start = atol(token);

        list = add_seq(list, se.seq_num, se.start, se.block);
    }

    // Close the file
    fclose(fp);

    //Open the gz file
    FILE *in;
    in = fopen(argv[3], "rb");
    if (NULL == in) {
        fprintf(stderr, "Error opening %s for reading\n", argv[2]);
    }

    int chunksize = 10000;
    int n = 20000000;
    int block;
    off_t seq_offset = 0;
    for (int i = 0; i < list->have; i++) {
        struct seq_entry* this = list->seq_entry + (i * sizeof(struct seq_entry)); /* Get the next seq_entry */
        if (this->seq_num == n) {
            seq_offset = this->start;
            block = this->block;
            break;
        }
    }

    //This is the index point we want
    struct point * idx_point = index->list + block;

    int len = extract(in, index, idx_point, seq_offset, buf, chunksize);
    if (len < 0)
        fprintf(stderr, "zran: extraction failed: %s error\n",
                len == Z_MEM_ERROR ? "out of memory" : "input corrupted");
    else {
        //fwrite(buf, 1, len, stdout);
        fprintf(stderr, "zran: extracted %d bytes from point %d\n", len, 10000);
    }

    /* clean up and exit */
    free_index(index);

    return 0;
}