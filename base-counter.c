#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <getopt.h>
#include <time.h>
#include <math.h>
#include <stdint.h>
#include <errno.h>
#include <pthread.h>

#define WINSIZE 32768U          /* sliding window size */
#define CHUNKSIZE 16384         /* file input buffer size */
#define MAXLINE 2 * WINSIZE
#define MSGSIZE 256
#define MAXTHREADS 16

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
int num_threads = 4;


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

/* task_args contains a pointer to the index struct and the seq chunk struct */
struct task_args {
    int tid;                                    /* Thread id */
    int start;                                  /* start seq chunk */
    int stop;                                   /* end seq chunk */
    char * filename;                            /* gz filename to read */
    struct access * index;                      /* Index point list */
    struct seq_list * list;                     /* Sequence point list */
};


/* struct extract_info contains all of the information
 * necessary for the call to extract() to do what it needs to
 * do */
struct extract_info{
    char * filename;
    struct access * index;
    struct point * this_block;
    off_t seq_offset;
    int nchunks;
};

/* keeps stats per thread */
struct stats{
    off_t A;
    off_t C;
    off_t G;
    off_t T;
    off_t N;
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

struct stats * extract(char * filename, struct access *index, struct point * this,
        off_t seq_offset, int nchunks)
{
    int ret, skip, seq_num;
    z_stream strm;
    unsigned char input[CHUNKSIZE];
    unsigned char discard[WINSIZE];
    unsigned char buf[WINSIZE];
    off_t line_num = 1;
    off_t totout = seq_num = 0;
    skip = 1;
    off_t buffsize = 2 * WINSIZE;
    FILE *in;
    struct stats * st = calloc(1, sizeof(struct stats));

    /* initialize file and inflate state to start there */
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    strm.avail_in = 0;
    strm.next_in = Z_NULL;
    ret = inflateInit2(&strm, -15);         /* raw inflate */
    if (ret != Z_OK)
        return NULL;

    off_t seek_offset = this->in - (off_t) (this->bits ? 1 : 0);

    //Open the gz file
    in = fopen(filename, "rb");
    if (NULL == in) {
        fprintf(stderr, "Error opening %s for reading\n", filename);
        exit(-1);
    }

    ret = fseeko(in, seek_offset, SEEK_SET);
    if (ret == -1) {
        perror("failed");
        printf("Error seeking to offset: %s\n", strerror(errno));
        exit(-1);
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
    seq_offset -= this->out;
    strm.avail_in = 0;
    do {
        /* define where to put uncompressed data, and how much */
        if (seq_offset > WINSIZE) {             /* skip WINSIZE bytes */
            strm.avail_out = WINSIZE;
            strm.next_out = discard;
            seq_offset -= WINSIZE;

        }
        else if (seq_offset > 0) {              /* last skip */
            strm.avail_out = (unsigned)seq_offset;
            strm.next_out = discard;
            seq_offset = 0;
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

                    //Check if it's a new line
                    if (strm.next_out[i] == '\n') {
                        // This is a new sequence
                        if ((line_num % 4) == 0) {
                            seq_num++;
                            /* If we've seen the total number of sequences we need to,
                             * return the buffer we've been building */
                            if ((nchunks > 0) && (seq_num % nchunks) == 0) {
                                /* This position is (totout - strm.avail_out) + i */
                                goto deflate_index_extract_ret;
                            }
                        }
                    line_num++;
                    } else if (line_num % 4 == 2) { //the sequence 
                        if (strm.next_out[i] == 'A')
                            st->A++;
                        else if (strm.next_out[i] == 'C')
                            st->C++;
                        else if (strm.next_out[i] == 'G')
                            st->G++;
                        else if (strm.next_out[i] == 'T')
                            st->T++;
                        else if (strm.next_out[i] == 'N')
                            st->N++;
                        else{
                            printf("Fatal. Encountered unknown nucleotide: %c\n", strm.next_out[i]);
                            exit(1);
                        }

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
            //strm.avail_out = WINSIZE;
            strm.next_out = buf;
            //skip = 0;                       /* only do this once */
            if (totout) {
                for (int i = 0; i < WINSIZE - strm.avail_out; i++) {
                    //Check if it's a new line
                    if (strm.next_out[i] == '\n') {
                        // This is a new sequence
                        if ((line_num % 4) == 0) {
                            seq_num++;
                            /* If we've seen the total number of sequences we need to,
                             * return the buffer we've been building */
                            if ((nchunks > 0) && (seq_num % nchunks) == 0) {
                                goto deflate_index_extract_ret;
                            }
                        }
                        line_num++;
                    } else if (line_num % 4 == 2) { //the sequence 
                        if (strm.next_out[i] == 'A')
                            st->A++;
                        else if (strm.next_out[i] == 'C')
                            st->C++;
                        else if (strm.next_out[i] == 'G')
                            st->G++;
                        else if (strm.next_out[i] == 'T')
                            st->T++;
                        else if (strm.next_out[i] == 'N')
                            st->N++;
                        else{
                            printf("Fatal. Encountered unknown nucleotide: %c\n", strm.next_out[i]);
                            exit(1);
                        }

                    }
                }
            }
            break;
        }

        /* do until offset reached and requested data read */
    } while (1);

    /* clean up and return the bytes read, or the negative error */
    deflate_index_extract_ret:

    fclose(in);
    (void)inflateEnd(&strm);

    return st;

}

void * task(void *arg) {

    off_t seq_offset, block_num;
    struct task_args ta = *(struct task_args *) arg;
    off_t out_size = WINSIZE;
    off_t total_bytes = 0;

    /* Get this sequence entry */
    //struct seq_entry * this_chunk = ta.list->seq_entry + (i * sizeof(struct seq_entry));
    struct seq_entry * this_chunk = ta.list->seq_entry + (ta.start * sizeof(struct seq_entry));
    int nchunks;
    if (ta.stop > 0) {
        nchunks = (ta.stop - ta.start) * idx_chunk_size;
    } else {
        nchunks = -1;
    }

    /* Offset into the file the start of this chunk of sequence reads is (uncompressed) */
    seq_offset = this_chunk->start;


    /* Block number that it corresponds with */
    block_num = this_chunk->block;

    /* Get the block structure */
    struct point * this_block = ta.index->list + block_num;
    struct stats * ret = extract(ta.filename, ta.index, this_block, seq_offset, nchunks);

    pthread_exit((void *) ret);
}

//Prints the usage information on error
void print_usage(char *argv[]) {
    fprintf(stderr, "Usage: %s [-n N_THREADS] GZIP-INDEX.IDX SEQUENCE-INDEX.SEQ-IDX GZIP_FILE \n", argv[0]);
}

void print_help(char *argv[]) {
    fprintf(stderr, "index-reader reads prebuilt index files for a gzipped FASTQ ");
    fprintf(stderr, "file to allow for parallel processing\n\n");
    fprintf(stderr, "Usage: %s [-n N_THREADS] GZIP-INDEX.IDX SEQUENCE-INDEX.SEQ-IDX GZIP_FILE \n", argv[0]);
    fprintf(stderr, "-n N_THREADS\tthe number of thread (<=16) to use (default 4)");
    fprintf(stderr, "-v\t\tenable verbose logging\n");
    fprintf(stderr, "GZIP-INDEX.IDX\t<index file> is a CSV index file for the GZIP FASTQ file's blocks\n");
    fprintf(stderr, "SEQUENCE-INDEX.IDX\t<index file> is a CSV index file for the GZIP FASTQ file's sequences\n");
    fprintf(stderr, "GZIP_FILE\t<gzip file> is a gzipped FASTQ file to index\n");
}

int main(int argc, char *argv[]) {

    FILE* fp;
    char line[MAXLINE];
    char* token;
    struct access * index = NULL;
    struct seq_list * list;
    struct point pt = {0};
    struct seq_entry se;
    unsigned char buf[CHUNKSIZE];
    char msg[MSGSIZE];
    pthread_t threads[MAXTHREADS];


    int opt;
    while ((opt = getopt(argc, argv, "c:ho:vn:")) != -1) {
        switch (opt) {
            case 'c': //chunk size
                idx_chunk_size = atoi(optarg);
                break;
            case 'v':
                GLOBAL_LEVEL = LOG_DEBUG;
                logger(LOG_DEBUG, "Debug logging enabled");
                break;
            case 'n':
                num_threads = atoi(optarg);
                break;
            default:
                print_usage(argv);
                return 1;
        }
    }

    if (optind >= argc) {
        print_usage(argv);
        return -1;
    }

    snprintf(msg, MSGSIZE, "Running with %d threads", num_threads);
    logger(LOG_INFO, msg);

    /* Open and read the GZIP index CSV file.
   * Add each struct point to the index access point list as we read it
   */
    fp = fopen(argv[optind], "r");
    if (fp == NULL) {
        printf("Error opening the file.\n");
        exit(1);
    }

    // Read each line of the file
    while (fgets(line, MAXLINE, fp) != NULL) {

        int decoded_len;
        char * decoded_string = NULL;

        /* Ignore comments, except the one with the sequence number hint */
        if (line[0] == '#') {

            char * substr = strstr(line, "#sequence_skip:");

            /* Didn't find the sequence */
            if (substr == NULL) {
                continue;
            }

            // Extract the number using strtok()
            char * token = strtok(substr, " ");

            if (token == NULL) {
                logger(LOG_ERROR, "Error: no token found after #sequence:");
                return 1;
            }

            token = strtok(NULL, " ");

            if (token == NULL) {
                logger(LOG_ERROR, "Error: no number found after #sequence:");
                return 1;
            }
            // Convert the number to an integer
            idx_chunk_size = atoi(token);

            snprintf(msg, MSGSIZE, "Read sequence number %d", idx_chunk_size);
            logger(LOG_DEBUG, msg);
            continue;
        }

        /* Block number */
        token = strtok(line, ",");

        /* out_offset */
        token = strtok(NULL, ",");
        pt.out = atol(token);

        /* in_offset */
        token = strtok(NULL, ",");
        pt.in = atol(token);

        /* bits offset */
        token = strtok(NULL, ",");
        pt.bits = atoi(token);

        /* window */
        token = strtok(NULL, ",");

        size_t encoded_len = strlen(token); //remove the \n

        // Calculate the maximum length of the decoded string
        size_t max_decoded_len = (encoded_len * 3);
        decoded_string = (char*)malloc(max_decoded_len + 1);

        if (decoded_string == NULL) {
            logger(LOG_ERROR, "Error: Memory allocation failed");
            return 1;
        }

        decoded_len = base64_decode(token, encoded_len, decoded_string, max_decoded_len);
        if ((decoded_len) < 0) {
            logger(LOG_ERROR, "Error: Decoded length < 0");
            exit(129);
        }

        decoded_string[WINSIZE] = '\0';
        strcpy(pt.window, decoded_string);

        index = add_read_point(index, pt.bits, pt.in, pt.out, decoded_string);
    }
    // Close the file
    fclose(fp);

    snprintf(msg, MSGSIZE, "Read %d points from %s", index->have, argv[optind]);
    logger(LOG_DEBUG, msg);
    optind++;

    /* Open and read the sequence index CSV file.
 * Add each sequence to the sequence list as we read it */
    fp = fopen(argv[optind], "r");
    if (fp == NULL) {
        logger(LOG_ERROR, "Error opening the sequence-index file");
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
    snprintf(msg, MSGSIZE, "Read %d points from %s", list->have, argv[optind]);
    logger(LOG_DEBUG, msg);
    optind++;

    /* If we have more threads than sequence chunks, reduce the number of threads */
    if (num_threads > list->have) {
        snprintf(msg, MSGSIZE, "Setting num_threads to %d from %d", list->have, num_threads);
        logger(LOG_INFO, msg);
        num_threads = list->have;
    } else if (num_threads > MAXTHREADS) {
        snprintf(msg, MSGSIZE, "Max number of threads is %d", MAXTHREADS);
        logger(LOG_INFO, msg);
        num_threads = MAXTHREADS;
    }


    struct task_args args[MAXTHREADS] = {0};
    // create threads
    for (int i = 0; i < num_threads; i++) {
        snprintf(msg, MSGSIZE, "Starting thread %d", i);
        logger(LOG_DEBUG, msg);

        double float_stride = (double)  list->have / num_threads;
        int stride = (int) floor(float_stride);
        int thread_start = stride * i;
        int thread_end;

        if ((stride * (i + 1)) < list->have) {
            thread_end = stride * (i + 1);
        } else {
            thread_end = list->have;
        }

        /* Set up the args struct for this thread */
        args[i].tid = i;
        args[i].filename = strndup(argv[optind], strlen(argv[optind]));
        args[i].index = index;
        args[i].list = list;
        args[i].start = thread_start;

        //last thread, read until end
        if (i == (num_threads - 1))
            args[i].stop = -1;
        else
            args[i].stop = thread_end;

        pthread_create(&threads[i], NULL, task, &args[i]);
    }

    struct stats * thread_results[MAXTHREADS];
    // wait for threads to finish
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], (void **) &thread_results[i]);
    }

    struct stats total_stats = {0};
    for (int i = 0; i < num_threads; i++) {
        total_stats.A += thread_results[i]->A;
        total_stats.C += thread_results[i]->C;
        total_stats.G += thread_results[i]->G;
        total_stats.T += thread_results[i]->T;
        total_stats.N += thread_results[i]->N;
    }

    printf("A: %ld C: %ld G: %ld T: %ld N: %ld Total: %ld\n", total_stats.A, total_stats.C, total_stats.G, total_stats.T, total_stats.N, total_stats.A + total_stats.C + total_stats.G + total_stats.T + total_stats.N);

    return 0;
}
