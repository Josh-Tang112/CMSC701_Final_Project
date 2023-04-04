#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <getopt.h>
#include <time.h>
#include <stdint.h>

#define WINSIZE 32768U          /* sliding window size */
#define CHUNKSIZE 16384         /* file input buffer size */
#define MAXLINE 2 * WINSIZE

int base64_decode(char* input, size_t input_len, char* output, size_t output_len) {
    int i, j;
    unsigned char c;
    unsigned char buffer[4];
    unsigned char temp[3];

    printf("output len: %d\n", output_len);

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
    int seq_start;      /* sequence number this index starts with */
    off_t out;          /* corresponding offset in uncompressed data */
    off_t in;           /* offset in input file of first full byte */
    int bits;           /* number of bits (1-7) from byte at in - 1, or 0 */
    unsigned char window[WINSIZE];  /* preceding 32K of uncompressed data */
};
/* access point list */
struct access {
    int seq_skip;       /* Number of sequences in each chunk */
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
static struct access *add_read_point(struct access *index, int seq_start, int bits,
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
    next->seq_start = seq_start;
    strcpy(next->window, window);
    index->have++;

    /* return list, possibly reallocated */
    return index;
}

static int extract(FILE *in, struct access *index, off_t seq_start, unsigned char *buf)
{
    off_t offset = 0;
    int ret, skip, len;
    len = CHUNKSIZE;
    z_stream strm;
    struct point *here, *this;
    unsigned char input[CHUNKSIZE];
    unsigned char discard[WINSIZE];

    if (seq_start == 0) {
        fprintf(stderr, "Haven't handled the 0 case yet!\n");
        exit(128);
    }

    /* find where in stream to start */
    here = index->list;
    for (int i = 0; i < index->have; i++) {
        here = index->list + i;
        if (here->seq_start == seq_start) {
            printf("%d %d %s\n", here->seq_start, here->in, here->window);
            break;
        }
    }

    printf("Here seq: %d\n", here->seq_start);

    /* initialize file and inflate state to start there */
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    strm.avail_in = 0;
    strm.next_in = Z_NULL;
    ret = inflateInit2(&strm, -15);         /* raw inflate */
    printf("After inflateInit2\n");
    if (ret != Z_OK)
        return ret;
    int off = here->in - (here->bits ? 1 : 0);
    printf("here->window: %s\n", here->window);
    printf("here->in: %ld\n", here->in);
    printf("Got off for the fseeko: %d\n", off);
    ret = fseeko(in, here->in - (here->bits ? 1 : 0), SEEK_SET);


    if (ret == -1)
        goto extract_ret;
    if (here->bits) {
        ret = getc(in);
        if (ret == -1) {
            ret = ferror(in) ? Z_ERRNO : Z_DATA_ERROR;
            goto extract_ret;
        }
        (void)inflatePrime(&strm, here->bits, ret >> (8 - here->bits));
    }
    (void)inflateSetDictionary(&strm, here->window, WINSIZE);

    //printf("here->out: %ld\n", here->out);
    /* skip uncompressed bytes until offset reached, then satisfy request */
    //offset -= here->out;
    offset = 0;
    printf("here->out: %ld\n", here->out);
    printf("offset: %ld\n", offset);
    strm.avail_in = 0;
    skip = 1;                               /* while skipping to offset */
    do {
        /* define where to put uncompressed data, and how much */
        if (offset == 0 && skip) {          /* at offset now */
            strm.avail_out = len;
            strm.next_out = buf;
            skip = 0;                       /* only do this once */
        }

        printf("next_in: %s\n", strm.next_in);
        printf("next_out: %s\n", strm.next_out);
        printf("avail_in: %d\n", strm.avail_in);
        printf("avail_out: %d\n", strm.avail_out);
        //printf("window: %s\n", here->window);

        printf("First 10: ");
        for (int i = 0; i < 10; i++) {
            printf("%c", here->window[i]);
        }
        printf("\n");
        printf("Last 10: ");
        for (int i = strlen(here->window)-10; i < strlen(here->window); i++) {
            printf("%c", here->window[i]);
        }
        printf("\n");


        /* uncompress until avail_out filled, or end of stream */
        do {
            if (strm.avail_in == 0) {
                printf("avail_in == 0, reading\n");
                strm.avail_in = fread(input, 1, CHUNKSIZE, in);
                printf("avail_in: %d\n", strm.avail_in);
                if (ferror(in)) {
                    ret = Z_ERRNO;
                    printf("ferror(in)\n");
                    goto extract_ret;
                }
                if (strm.avail_in == 0) {
                    printf("avail_in == 0 from read\n");
                    ret = Z_DATA_ERROR;
                    goto extract_ret;
                }
                strm.next_in = input;
            }

            //printf("next_in: %s\n", strm.next_in);
            //printf("next_out: %x\n", strm.next_out);

            printf("next_out: %s\n", strm.next_out);
            printf("next_in: ");
            for (int i = 0; i < 10; i ++){
                printf("%02x", strm.next_in[i]);
            }
            printf("\n");

            printf("avail_in: %d\n", strm.avail_in);
            printf("avail_out: %d\n", strm.avail_out);

            ret = inflate(&strm, Z_NO_FLUSH);       /* normal inflate */

            printf("Inflate ret: %d\n", ret);

            if (ret == Z_NEED_DICT)
                ret = Z_DATA_ERROR;
            if (ret == Z_MEM_ERROR || ret == Z_DATA_ERROR || ret == Z_STREAM_ERROR)
                goto extract_ret;
            if (ret == Z_STREAM_END)
                break;
        } while (strm.avail_out != 0);

        /* if reach end of stream, then don't keep trying to get more */
        if (ret == Z_STREAM_END)
            break;

        /* do until offset reached and requested data read, or stream ends */
    } while (skip);

    /* compute number of uncompressed bytes read after offset */
    ret = skip ? 0 : len - strm.avail_out;

    /* clean up and return bytes read or error */
    extract_ret:
    (void)inflateEnd(&strm);
    return ret;
}

int main(int argc, char *argv[]) {
    FILE* fp;
    char line[MAXLINE];
    char* token;
    struct access * index;
    struct point pt;
    unsigned char buf[CHUNKSIZE];

    if (argc != 3) {
        fprintf(stderr, "usage: %s index.csv file.gz\n", argv[0]);
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
        pt.seq_start = atoi(token);

        token = strtok(NULL, ",");
        pt.out = atoi(token);

        token = strtok(NULL, ",");
        pt.in = atoi(token);

        token = strtok(NULL, ",");
        pt.bits = atoi(token);

        token = strtok(NULL, ",");


        printf("Last token: %s\n", token);
        size_t encoded_len = strlen(token);

        // Calculate the maximum length of the decoded string
        size_t max_decoded_len = (encoded_len * 3);
        printf("strlen(last token): %d\n", encoded_len);
        printf("max_decoded_len: %d\n", max_decoded_len);
        decoded_string = (char*)malloc(max_decoded_len + 1);

        if (decoded_string == NULL) {
            printf("Error: Memory allocation failed.\n");
            return 1;
        }

        decoded_len = base64_decode(token, encoded_len, decoded_string, max_decoded_len);
        printf("Decoded len: %d\n", decoded_len);
        if ((decoded_len) < 0) {
            fprintf(stderr, "Error: Decoded_len < 0\n");
            exit(129);
        }
        decoded_string[WINSIZE] = '\0';
        strcpy(pt.window, decoded_string);

        printf("here\n");
        printf("%d %s %s\n", pt.seq_start, pt.window, decoded_string);
/*
        printf("Access Point:\nStart seq_num:%d\nOut: %ld\nIn: %ld\nBits: %d\nWindow: %ld\n",
               pt.seq_start, pt.out, pt.in, pt.bits, strlen(pt.window));
*/


        index = add_read_point(index, pt.seq_start, pt.bits, pt.in, pt.out, decoded_string);
    }

    // Close the file
    fclose(fp);
/*
    printf("Read %d element index table\n", index->have);
    for (int i = 0; i < index->have; i++) {
        struct point * this = index->list + i;
        printf("i: %d seq: %d %s\n", i, this->seq_start, this->window);
    }
*/

    //Open the gz file
    FILE *in;
    in = fopen(argv[2], "rb");
    if (NULL == in) {
        fprintf(stderr, "Error opening %s for reading\n", argv[2]);
    }

    //Test the lookup
    struct point * this;
    for (int i = 0; i < index->have; i++) {
        this = index->list + i;
        printf("i: %d, start: %d, window: %s\n", i, this->seq_start, this->window);
        printf("strlen(window): %d\n", strlen(this->window));
        break;
    }
    printf("i: %d, start: %d, window: %s\n", 0, this->seq_start, this->window);
    int len = extract(in, index, this->seq_start, buf);
    //int len = 0;
    if (len < 0)
        fprintf(stderr, "zran: extraction failed: %s error\n",
                len == Z_MEM_ERROR ? "out of memory" : "input corrupted");
    else {
        fwrite(buf, 1, len, stdout);
        fprintf(stderr, "zran: extracted %d bytes from point %d\n", len, 10000);
    }

    /* clean up and exit */
    free_index(index);

    return 0;
}