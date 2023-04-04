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

enum log_level_t {
    LOG_NOTHING,
    LOG_CRITICAL,
    LOG_ERROR,
    LOG_WARNING,
    LOG_INFO,
    LOG_DEBUG,
    LOG_TRACE
};

enum log_level_t GLOBAL_LEVEL = LOG_WARNING;
#define READ_SIZE 1024
int line_num = 1;
int seq_num = 0;
int idx_chunk_size = 10000;
char *output_file = "output.idx";
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

/* BEGIN ZRAN CODE NEED CITE */

/* access point entry */
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

/* Add an entry to the access point list.  If out of memory, deallocate the
   existing list and return NULL. */
static struct access *addpoint(struct access *index, int seq_start, int bits,
                              off_t in, off_t out, unsigned left, unsigned char *window)
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
    if (left)
        memcpy(next->window, window + WINSIZE - left, left);
    if (left < WINSIZE)
        memcpy(next->window + left, window, WINSIZE - left);
    index->have++;

    /* return list, possibly reallocated */
    return index;
}

/* END ZRAN CODE */

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

void logger (enum log_level_t level, const char* message) {
    if (level <= GLOBAL_LEVEL) {
        time_t now;
        time (&now);
        fprintf (stderr,"%ld [%s]: %s\n", now, level_to_string(level), message);
    }
}

//Prints the usage information on error
void print_usage(char *argv[]) {
    fprintf(stderr, "Usage: %s -c CHUNKSIZE GZIP_FILE\n", argv[0]);
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

/* write_index
 * @brief: writes the index file to the specified output file
 * @params:
 * fname (string): Output file name
 * infile (string): The file we're parsing
 * index (struct access *): The access point index pointer
 * @returns: 0 on success, < 0 on failure
 */
int write_index(char * fname, char * infile, struct access * index) {
    FILE *fp;
    time_t t;
    char header[256];

    /* Check that the index is NULL first */
    if (NULL == index) {
        logger(LOG_ERROR, "Index was NULL");
        return -1;
    }


    // Open file for writing, or create it if it doesn't exist
    fp = fopen(fname, "w");
    if (NULL == fp) {
        logger(LOG_CRITICAL, "Failed to open output file for writing");
        return -1;
    }

    // Write the index file header
    t = time(NULL);
    snprintf(header, sizeof(header),
             "#time: %ld\n#input: %s\n#sequence_skip: %d\n#seq_num,out_offset,in_offset,bits_in,window\n",
             (long) t, infile, index->seq_skip);
    fputs(header, fp);

    // Iterate over each of the access points in the index, writing the
    // info to the index file
    for (int i = 0; i < index->have; i++) {
        size_t output_len;
        char line[MAXLINE];
        struct point * pt = index->list + i; /* Get the next point */
        char * b64 = base64_encode(pt->window,
                      strlen((const char *) pt->window),
                      &output_len); /* Base64 encode the window */

        //Create the output line and write it to the file
        sprintf(line, "%d,%ld,%ld,%d,%s\n",
                pt->seq_start, pt->out, pt->in, pt->bits, b64);
        fputs(line,fp);
    }

    // Close the file
    fclose(fp);

    return 0;
}

int main(int argc, char *argv[]) {
    int opt, len;
    char *filename;
    struct access *index = NULL;

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

    FILE *fp= fopen(filename, "rb");
    if (!fp) {
        snprintf(err_str, 100, "Fatal error; failed to open %s\n", filename);
        logger(LOG_ERROR, err_str);
        return 1;
    }

    z_stream zstrm;
    zstrm.zalloc = Z_NULL;
    zstrm.zfree = Z_NULL;
    zstrm.opaque = Z_NULL;
    zstrm.avail_in = 0;
    zstrm.next_in = Z_NULL;
    if (inflateInit2(&zstrm, MAX_WBITS + 32) != Z_OK) {
        printf("Error initializing z_stream structure\n");
        return 1;
    }

    char in_buf[CHUNKSIZE];
    char out_buf[CHUNKSIZE];
    char window[WINSIZE];
    int ret, flush, totin, totout;
    int window_idx = totin = totout = 0;
    index = NULL; //Will be allocated by first call to addpoint() as in zran
    do {
        zstrm.avail_in = fread(in_buf, 1, CHUNKSIZE, fp);
        if (ferror(fp)) {
            printf("Error reading file\n");
            return 1;
        }
        flush = feof(fp) ? Z_FINISH : Z_SYNC_FLUSH;
        zstrm.next_in = (Bytef *)in_buf;

        do {
            zstrm.avail_out = CHUNKSIZE;
            zstrm.next_out = (Bytef *)out_buf;

            totin += zstrm.avail_in;   //add stuff we're inflating
            totout += zstrm.avail_out;
            ret = inflate(&zstrm, flush);
            totin -= zstrm.avail_in;   //subtract any that didn't get inflated
            totout -= zstrm.avail_out;


            if (ret == Z_STREAM_ERROR) {
                printf("Error inflating data\n");
                return 1;
            }
            if (ret == Z_NEED_DICT || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
                printf("Error inflating data: %s\n", zError(ret));
                return 1;
            }
            int i, start = 0;
            for (i = 0; i < CHUNKSIZE - zstrm.avail_out; i++) {

                /* Need to keep track of the last WINSIZE characters*/
                if (window_idx < WINSIZE) {
                    window[window_idx] = out_buf[i];
                    window_idx++;
                } else {
                    memmove(window, window + 1, WINSIZE - 1);  // Slide window
                    window[WINSIZE - 1] = out_buf[i];  // Add new character to end of window
                }

                /* We've reached a new line in the input file
                 * 0. (debug) print it
                 * 1. increment line counter
                 * 2. if line % 4 == 0, increment sequence counter
                 */

                if (out_buf[i] == '\n') {
                    if ((line_num % 4)  == 0) {
                        seq_num++;
                        out_buf[i] = '\0';

                        /* If we've seen idx_chunk_size number of sequences, make an index */
                        if ((seq_num % idx_chunk_size) == 0) {
                            char msg[100];
                            sprintf(msg, "Creating index for seqnum %d", seq_num);
                            logger(LOG_DEBUG, msg);
                            index = addpoint(index, seq_num, zstrm.data_type & 7,
                                             totin, totout, zstrm.avail_out, window);
                        }
                    }
                    start = i + 1;
                    line_num++;

                }
            }
        } while (zstrm.avail_out == 0);
    } while (flush != Z_FINISH);

    index->seq_skip = idx_chunk_size;
    inflateEnd(&zstrm);
    fclose(fp);

    if (write_index(output_file, filename, index) < 0) {
        logger(LOG_ERROR, "Error writing output file; exiting");
        return -1;
    }

    return 0;

}

