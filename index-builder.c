#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <getopt.h>
#include <time.h>
#include <stdint.h>
#include "deflate.h"

#define MSGSIZE 256
#define WINSIZE 32768U          /* sliding window size */
#define CHUNKSIZE 16384         /* file input buffer size */
#define MAXLINE 2 * WINSIZE

enum log_level_t {
    LOG_NOTHING,
    LOG_CRITICAL,
    LOG_ERROR,
    LOG_WARNING,
    LOG_INFO,
    LOG_DEBUG,
    LOG_TRACE
};

enum log_level_t GLOBAL_LEVEL = LOG_INFO;
int idx_chunk_size = 10000;
int line_num = 1; //Want the mod 4 maths to work out
off_t seq_num = 0;
int need_idx = 0;
int block_num = 0;
char *output_file = "output";
char err_str[100];

static const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *base64_encode(const unsigned char *data, size_t input_length, size_t *output_length) {
    *output_length = 4 * ((input_length + 2) / 3);

    char *encoded_data = malloc(*output_length);
    if (encoded_data == NULL) return NULL;

    for (size_t i = 0, j = 0; i < input_length;) {
        uint32_t octet_a = i < input_length ? (unsigned char)data[i++] : 0;
        uint32_t octet_b = i < input_length ? (unsigned char)data[i++] : 0;
        uint32_t octet_c = i < input_length ? (unsigned char)data[i++] : 0;

        uint32_t triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;

        encoded_data[j++] = base64_chars[(triple >> 3 * 6) & 0x3F];
        encoded_data[j++] = base64_chars[(triple >> 2 * 6) & 0x3F];
        encoded_data[j++] = base64_chars[(triple >> 1 * 6) & 0x3F];
        encoded_data[j++] = base64_chars[(triple >> 0 * 6) & 0x3F];
    }

    static const char padding_char = '=';
    switch (input_length % 3) {
        case 1:
            encoded_data[*output_length - 1] = padding_char;
            encoded_data[*output_length - 2] = padding_char;
            break;
        case 2:
            encoded_data[*output_length - 1] = padding_char;
            break;
    }

    return encoded_data;
}

/* level_to_string is a utility to toggle log levels */
const char* level_to_string(enum log_level_t level) {
    switch (level) {
        case LOG_CRITICAL:
            return "CRITICAL";
        case LOG_ERROR:
            return "ERROR";
        case LOG_WARNING:
            return "WARNING";
        case LOG_INFO:
            return "INFO";
        case LOG_DEBUG:
            return "DEBUG";
        default:
            return "UNKNOWN";
    }
}

/* Logger prints log messages at the specified granularity to stdout */
void logger(enum log_level_t level, const char* message) {
    if (level <= GLOBAL_LEVEL) {
        time_t now;
        time (&now);
        fprintf (stderr,"%ld [%s]: %s\n", now, level_to_string(level), message);
    }
}

/* seq_entry defines the data needed to store a sequence number's offset into
 * 1) The block it's a part of
 * 2) The number of bytes from the start of the (uncompressed file) it is. This
 * data is used to move from the start of the block to the start of the sequence
 * when reading */
struct seq_entry {
    off_t seq_num;       /* Sequence number */
    off_t start;       /* Offset from the start the uncompressed file */
    int block;         /* Block number this sequence starts in */
};

/* seq_list is a list of seq_entry's that we maintain to write out a
 * sequence index */
struct seq_list {
    int have;           /* Number of seq_entries */
    int size;           /* Number of seq_entries we can have */
    void *seq_entry;    /* List of seq_entries */
};

/* deflate_index is an access point list. This code was taken from
 * zran.c, written by Mark Adler (https://github.com/madler/zlib/blob/master/examples/zran.c) */
struct deflate_index {
    int have;           /* number of list entries */
    int gzip;           /* 1 if the index is of a gzip file, 0 if it is of a
                           zlib stream */
    off_t length;       /* total length of uncompressed data */
    void *list;         /* allocated list of entries */
};

static struct seq_list * add_seq(struct seq_list * list, off_t seqNum,
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

/* point is an entry in the access point list. This code was taken from
 * zran.c, written by Mark Adler (https://github.com/madler/zlib/blob/master/examples/zran.c) */
struct point {
    off_t out;          /* corresponding offset in uncompressed data */
    off_t in;           /* offset in input file of first full byte */
    int bits;           /* number of bits (1-7) from byte at in-1, or 0 */
    unsigned char window[WINSIZE];  /* preceding 32K of uncompressed data */
};


/* deflate_index_free() frees all of the heap-allocated memory for a deflate_index. This code
 * was taken from zran.c, written by Mark Adler (https://github.com/madler/zlib/blob/master/examples/zran.c) */
void deflate_index_free(struct deflate_index *index) {
    if (index != NULL) {
        free(index->list);
        free(index);
    }
}

/*  addpoint() adds a point to a deflate_index. This code was taken from
 * was taken from zran.c, written by Mark Adler (https://github.com/madler/zlib/blob/master/examples/zran.c)
 * Add an entry to the access point list. If out of memory, deallocate the
   existing list and return NULL. index->gzip is the allocated size of the
   index in point entries, until it is time for deflate_index_build() to
   return, at which point gzip is set to indicate a gzip file or not.
 */
static struct deflate_index *addpoint(struct deflate_index *index, int bits,
                                      off_t in, off_t out, unsigned left,
                                      unsigned char *window) {
    struct point *next;

    /* if list is empty, create it (start with eight points) */
    if (index == NULL) {
        index = malloc(sizeof(struct deflate_index));
        if (index == NULL) return NULL;
        index->list = malloc(sizeof(struct point) << 3);
        if (index->list == NULL) {
            free(index);
            return NULL;
        }
        index->gzip = 8;
        index->have = 0;
    }

        /* if list is full, make it bigger */
    else if (index->have == index->gzip) {
        index->gzip <<= 1;
        next = realloc(index->list, sizeof(struct point) * index->gzip);
        if (next == NULL) {
            deflate_index_free(index);
            return NULL;
        }
        index->list = next;
    }

    /* fill in entry and increment how many we have */
    next = (struct point *) (index->list) + index->have;
    next->bits = bits;
    next->in = in;
    next->out = out;
    if (left)
        memcpy(next->window, window + WINSIZE - left, left);
    if (left < WINSIZE)
        memcpy(next->window + left, window, WINSIZE - left);

    char msg[MSGSIZE];
    snprintf(msg, MSGSIZE, "Making index %d in: %lu out: %lu", index->have, next->in, next->out);
    logger(LOG_DEBUG, msg);

    index->have++;
    /* return list, possibly reallocated */
    return index;
}

/* END ZRAN CODE */


//Prints the usage information on error
void print_usage(char *argv[]) {
    fprintf(stderr, "Usage: %s [-c CHUNKSIZE] GZIP_FILE\n", argv[0]);
}

void print_help(char *argv[]) {
    fprintf(stderr, "index-builder builds an index into a gzipped FASTQ ");
    fprintf(stderr, "file to allow for parallel processing\n\n");
    fprintf(stderr, "Usage: %s [-c CHUNKSIZE] [-o OUTFILE] GZIP_FILE\n", argv[0]);
    fprintf(stderr, "-c CHUNKSIZE\tthe integer chunk size with which to ");
    fprintf(stderr, "store indexes into the gzip file (default 10000)\n");
    fprintf(stderr, "-o OUTFILE\tthe name of the output index file to ");
    fprintf(stderr, "write (default 'output.idx')\n");
    fprintf(stderr, "-v\t\tenable verbose logging\n");
    fprintf(stderr, "GZIP_FILE\t<gzip file> is a gzipped FASTQ file to index\n");
}

int write_seqs(char * fname, char * infile, struct seq_list* list) {
    FILE *fp;
    time_t t;
    char fullname[256];
    char header[256];

    // Open file for writing, or create it if it doesn't exist
    sprintf(fullname, "%s.seq-idx", fname);
    fp = fopen(fullname, "w");
    if (NULL == fp) {
        logger(LOG_CRITICAL, "Failed to open output file for writing");
        return -1;
    }

    // Write the sequence index file header
    t = time(NULL);
    snprintf(header, sizeof(header),
             "#time: %ld\n#input: %s\n#sequence_skip: %d\n#seq_num,block_num,out_offset\n",
             (long) t, infile, idx_chunk_size);
    fputs(header, fp);

    // Iterate over each of the access points in the index, writing the
    // info to the index file
    for (int i = 0; i < list->have; i++) {
        char line[MAXLINE];
        struct seq_entry* this = list->seq_entry + (i * sizeof(struct seq_entry)); /* Get the next seq_entry */
        sprintf(line, "%lu,%d,%lu\n", this->seq_num, this->block, this->start);
        fputs(line,fp);
    }

    char msg[MSGSIZE * 2];
    snprintf(msg, MSGSIZE * 2, "Wrote %d entries to sequence index file %s", list->have, fullname);
    logger(LOG_INFO, msg);
    // Close the file
    fclose(fp);
    return 0;
}


/* write_index
 * @brief: writes the index file to the specified output file
 * @params:
 * fname (string): Output file name
 * infile (string): The file we're parsing
 * index (struct deflate_index *): The access point index pointer
 * @returns: 0 on success, < 0 on failure
 */
int write_index(char * fname, char * infile, struct deflate_index * index) {
    FILE *fp;
    time_t t;
    char fullname[256];
    char header[256];

    /* Check that the index is NULL first */
    if (NULL == index) {
        logger(LOG_ERROR, "Index was NULL");
        return -1;
    }

    // Open file for writing, or create it if it doesn't exist
    sprintf(fullname, "%s.idx", fname);
    fp = fopen(fullname, "w");
    if (NULL == fp) {
        logger(LOG_CRITICAL, "Failed to open output file for writing");
        return -1;
    }

    // Write the index file header
    t = time(NULL);
    snprintf(header, sizeof(header),
             "#time: %ld\n#input: %s\n#sequence_skip: %d\n#block_num,out_offset,in_offset,bits_in,window\n",
             (long) t, infile, idx_chunk_size);
    fputs(header, fp);

    // Iterate over each of the access points in the index, writing the
    // info to the index file
    for (int i = 0; i < index->have; i++) {
        size_t output_len;
        char line[MAXLINE];
        struct point * pt = index->list + (i * sizeof(struct point)); /* Get the next point */
        char * b64 = base64_encode(pt->window,
                      strlen((const char *) pt->window),
                      &output_len); /* Base64 encode the window */

        //Create the output line and write it to the file
        sprintf(line, "%d,%ld,%ld,%d,%s\n",
                i,pt->out, pt->in, pt->bits, b64);
        fputs(line,fp);
    }

    char msg[MSGSIZE * 2];
    snprintf(msg, MSGSIZE * 2, "Wrote %d entries to gzip index file %s", index->have, fullname);
    logger(LOG_INFO, msg);

    // Close the file
    fclose(fp);

    return 0;
}

int main(int argc, char *argv[]) {
    int opt;
    char *filename;
    while ((opt = getopt(argc, argv, "c:ho:v")) != -1) {
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
            case 'v':
                GLOBAL_LEVEL = LOG_DEBUG;
                logger(LOG_DEBUG, "Debug logging enabled");
                break;
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

    time_t start_time = time(NULL);
    FILE *fp= fopen(filename, "rb");
    if (!fp) {
        snprintf(err_str, 100, "Fatal error; failed to open %s\n", filename);
        logger(LOG_ERROR, err_str);
        return 1;
    }

    int ret;
    off_t totin, totout;        /* our own total counters to avoid 4GB limit */
    struct deflate_index *index;    /* access points being generated */
    struct seq_list *seqList;
    z_stream strm;
    unsigned char input[CHUNKSIZE];
    unsigned char window[WINSIZE];

    /* initialize inflate */
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    strm.avail_in = 0;
    strm.next_in = Z_NULL;
    ret = inflateInit2(&strm, 47);      /* automatic zlib or gzip decoding */
    if (ret != Z_OK)
        return ret;

    /* inflate the input, maintain a sliding window, and build an index -- this
       also validates the integrity of the compressed data using the check
       information in the gzip or zlib stream */
    totin = totout = 0;
    index = NULL;               /* will be allocated by first addpoint() */
    strm.avail_out = 0;
    do {

        strm.avail_in = fread(input, 1, CHUNKSIZE, fp);
        if (ferror(fp)) {
            ret = Z_ERRNO;
            //goto build_index_error;
            return ret;
        }
        if (strm.avail_in == 0) {
            ret = Z_DATA_ERROR;
            //goto build_index_error;
            return ret;
        }
        strm.next_in = input;

        /* process all of that, or until end of stream */
        do {
            /* reset sliding window if necessary */
            if (strm.avail_out == 0) {
                strm.avail_out = WINSIZE;
                strm.next_out = window;
                /* If we haven't read any actual data bytes yet, this
                 * is header goop. Ignore */
                if (totout) {

                    /* Read through the decompressed data in next_out */
                    for (int i = 0; i < strm.avail_out; i++){

                        /* If there's a new line, we care about that because they have meaning
                         * in FASTQ files */
                        if (strm.next_out[i] == '\n') {

                            /* If this is the 4th one, it's a new sequence */
                            if ((line_num % 4) == 0) {

                                seq_num++;

                                /* If this is a multiple of the chunk size, make an
                                 * entry in the sequence index */
                                if ((seq_num % idx_chunk_size) == 0) {

                                    char msg[MSGSIZE];
                                    snprintf(msg, MSGSIZE, "Making sequence index entry for sequence number %lu", seq_num);
                                    logger(LOG_DEBUG, msg);
                                    /* This position is (totout - strm.avail_out) + i */
                                    seqList = add_seq(seqList, seq_num, (totout - strm.avail_out) + i + 1, block_num);
                                    need_idx = 1;
                                }
                            }
                            line_num++;
                        }
                    }
                }
            }



            /* inflate until out of input, output, or at end of block --
               update the total input and output counters */
            totin += strm.avail_in;
            totout += strm.avail_out;
            ret = inflate(&strm, Z_BLOCK);      /* return at end of block */
            totin -= strm.avail_in;
            totout -= strm.avail_out;
            if (ret == Z_NEED_DICT)
                ret = Z_DATA_ERROR;
            if (ret == Z_MEM_ERROR || ret == Z_DATA_ERROR)
                //TODO handle errors
                return ret;
            if (ret == Z_STREAM_END)
                break;


            /* if at end of block, consider adding an index entry (note that if
               data_type indicates an end-of-block, then all of the
               uncompressed data from that block has been delivered, and none
               of the compressed data after that block has been consumed,
               except for up to seven bits) -- the totout == 0 provides an
               entry point after the zlib or gzip header, and assures that the
               index always has at least one access point; we avoid creating an
               access point after the last block by checking bit 6 of data_type
             */
            if ((strm.data_type & 128) && !(strm.data_type & 64) &&
                (totout == 0 || need_idx)) {
                index = addpoint(index, strm.data_type & 7, totin,
                                 totout, strm.avail_out, window);
                char msg[MSGSIZE];
                snprintf(msg, MSGSIZE, "Adding index checkpoint for block %d\n", block_num);
                if (totout)
                    block_num++;
                if (index == NULL) {
                    ret = Z_MEM_ERROR;
                    return ret;
                }
                need_idx = 0;
            }
        } while (strm.avail_in != 0);
    } while (ret != Z_STREAM_END);

    /* Write the GZIP index file with suffix ".idx" */
    if (write_index(output_file, filename, index) < 0) {
        logger(LOG_ERROR, "Error writing gzip index file; exiting");
        return -1;
    }

    /* Write the sequence index file with suffix ".seq-idx" */
    if (write_seqs(output_file, filename, seqList) < 0) {
        logger(LOG_ERROR, "Error writing sequence index file; exiting");
        return -1;
    }

    time_t end_time = time(NULL);
    double elapsed_time = difftime(end_time, start_time);
    char msg[MSGSIZE];
    snprintf(msg, MSGSIZE, "Time elapsed creating index files: %f seconds", elapsed_time);
    logger(LOG_INFO, msg);

    return 0;

}

