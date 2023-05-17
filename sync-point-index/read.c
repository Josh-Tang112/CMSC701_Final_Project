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
#define MAXTHREADS 16

struct index_entry {
    char is_last_entry;
    uint64_t byte_offset;
    uint64_t uncompressed_len;
    uint64_t last_seq;
};

typedef struct {
    uint64_t size;
    struct index_entry* index; 
} fastq_gz_index;

long fsize(FILE* file) {
    long pos = ftell(file);
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, pos, SEEK_SET);
    return size;
}

fastq_gz_index* read_index_file(const char* index_filename) {
    FILE* fp;
    uint64_t num_entries;
    fastq_gz_index* index;

    if (!(fp = fopen(index_filename, "rb"))) {
        printf("Could not open file %s for reading\n", index_filename);
        exit(-1);
    }

    // For a file with n sync points, there are n + 2 index entries (we add dummy entries at
    // the beginning and end to make calculations easier) and n + 1 chunks (we can start reading
    // from the beginning of the file too). The dummy entry at the beginning consists of all
    // zeros, the dummy entry at the end gives information at the end of the file but is not
    // a valid place to start reading. The ending dummy entry is already contained in the index
    // file, since it is added during index building.
    index = malloc(sizeof(fastq_gz_index));
    index->size = (fsize(fp) / (sizeof(uint64_t) * 3)) + 1;
    index->index = malloc(index->size * sizeof(struct index_entry));

    index->index[0].is_last_entry = index->index[0].byte_offset = index->index[0].uncompressed_len = index->index[0].last_seq = 0;
    for (uint64_t i = 1; i < index->size; i++) {
        fread(&index->index[i].byte_offset, 1, sizeof(uint64_t), fp);
        fread(&index->index[i].uncompressed_len, 1, sizeof(uint64_t), fp);
        fread(&index->index[i].last_seq, 1, sizeof(uint64_t), fp);
        index->index[i].is_last_entry = (i == index->size - 1) ? 1 : 0;
        printf("i: %llu | byte_offset: %llu | uncompressed_len: %llu | last_seq %llu\n", 
                i, index->index[i].byte_offset, index->index[i].uncompressed_len, index->index[i].last_seq);
    }

    fclose(fp);

    return index;
}

char* extract_reads(const char* fastq_gz_filename, fastq_gz_index* index, uint64_t start_read, uint64_t end_read) {
    FILE* fp;
    struct index_entry* start_entry;
    struct index_entry* curr_entry = index->index;
    z_stream stream;
    uint64_t compressed_data_len, reads_len;
    char* compressed_data_buffer;
    char* reads_buffer;

    if (!(fp = fopen(fastq_gz_filename, "rb"))) {
	    printf("Cannot open file %s for reading\n", fastq_gz_filename);
	    exit(-1);
	}
    
    if (start_read > end_read) {
        printf("Start read %llu cannot be larger than end read %llu\n", start_read, end_read);
        exit(-1);
    }
    else if (start_read > index->index[index->size - 1].last_seq) {
        printf("Start read %llu cannot be larger than number of reads %llu in the FASTQ file\n", start_read, index->index[index->size - 1].last_seq);
        exit(-1);
    }
    else if (end_read > index->index[index->size - 1].last_seq) {
        end_read = index->index[index->size - 1].last_seq;
    }

    // Sync point is AFTER everything it's logging, so need to refer
    // to previous index entry for byte offset and to derive lengths
    while (!curr_entry->is_last_entry && start_read > (curr_entry + 1)->last_seq) { curr_entry++; }
    start_entry = curr_entry;
    while (curr_entry->last_seq < end_read) { curr_entry++; }

    compressed_data_len = curr_entry->byte_offset - start_entry->byte_offset;
    reads_len = curr_entry->uncompressed_len - start_entry->uncompressed_len;

    if (!(compressed_data_buffer = malloc(compressed_data_len))) {
        printf("Memory allocation for gzip contents failed\n");
        exit(-1);
    }
    if (!(reads_buffer = malloc(reads_len + 1))) {
        printf("Memory allocation for decompressed data failed\n");
        exit(-1);
    }

	fseek(fp, start_entry->byte_offset, SEEK_SET);
    stream.avail_in = fread(compressed_data_buffer, 1, compressed_data_len, fp);
    stream.avail_out = reads_len;
    stream.next_in = (Bytef *) compressed_data_buffer;
    stream.next_out = (Bytef *) reads_buffer;
    stream.zalloc = Z_NULL;
    stream.zfree = Z_NULL;
    stream.opaque = Z_NULL;
    if (inflateInit2(&stream, (start_entry->byte_offset) ? -15 : 47) != Z_OK) {
        printf("Error initializing stream state for decompression\n");
        exit(-1);
    }

	inflate(&stream, Z_FINISH);

    uint64_t start_read_start_pos;
    uint64_t end_read_end_pos;
    uint64_t newlines_from_start = (start_read - start_entry->last_seq - 1) * 4;
    uint64_t newlines_from_end = ((curr_entry->last_seq - end_read) * 4) + 1;

    uint64_t i;
    uint64_t newlines = 0;
    for (i = 0; i < reads_len && newlines < newlines_from_start; i++) { 
        if (reads_buffer[i] == '\n') newlines++;
    }
    start_read_start_pos = i;
    newlines = 0;
    for (i = reads_len - 1; i >= 0 && newlines < newlines_from_end; i--) { 
        if (reads_buffer[i] == '\n') newlines++;
    }
    end_read_end_pos = i + 1;

    memmove(reads_buffer, reads_buffer + start_read_start_pos, end_read_end_pos - start_read_start_pos + 1);
    reads_buffer[end_read_end_pos - start_read_start_pos + 1] = 0;

    printf("%s", reads_buffer);

    return reads_buffer;
}

// chunks are 1-indexed
char* extract_chunks(const char* fastq_gz_filename, fastq_gz_index* index, uint64_t start_chunk, uint64_t n) {
    FILE* fp;
    struct index_entry* start_entry;
    struct index_entry* end_entry;
    z_stream stream;
    uint64_t compressed_data_len, reads_len;
    char* compressed_data_buffer;
    char* reads_buffer;

    if (!(fp = fopen(fastq_gz_filename, "rb"))) {
	    printf("Cannot open file %s for reading\n", fastq_gz_filename);
	    exit(-1);
	}

    if (start_chunk >= index->size) {
        printf("Start chunk %llu cannot exceed the number of chunks %llu in the file\n", start_chunk, index->size - 1);
        exit(-1);
    }

    start_entry = index->index + start_chunk - 1;
    end_entry = (start_chunk + n >= index->size) ? index->index + index->size - 1 : start_entry + n;

    compressed_data_len = end_entry->byte_offset - start_entry->byte_offset;
    reads_len = end_entry->uncompressed_len - start_entry->uncompressed_len;

    if (!(compressed_data_buffer = malloc(compressed_data_len))) {
        exit(-1);
    }
    if (!(reads_buffer = malloc(reads_len + 1))) {
        exit(-1);
    }

	fseek(fp, start_entry->byte_offset, SEEK_SET);
    stream.avail_in = fread(compressed_data_buffer, 1, compressed_data_len, fp);
    stream.avail_out = reads_len;
    stream.next_in = (Bytef *) compressed_data_buffer;
    stream.next_out = (Bytef *) reads_buffer;
    stream.zalloc = Z_NULL;
    stream.zfree = Z_NULL;
    stream.opaque = Z_NULL;
    if (inflateInit2(&stream, (start_entry->byte_offset) ? -15 : 47) != Z_OK) {
        printf("Error initializing stream state for decompression\n");
        exit(-1);
    }

	inflate(&stream, Z_FINISH);
    reads_buffer[reads_len - stream.avail_out] = 0;

    //fwrite(reads_buffer, 1, reads_len - stream.avail_out, stdout);

    return reads_buffer;
}

struct task_args {
    uint64_t start;
    uint64_t nchunks;
    const char* fastq_gz_filename; 
    fastq_gz_index* index;
};

void* task(void* args) {
    struct task_args* task_args = (struct task_args*) args;
    char* reads = extract_chunks(task_args->fastq_gz_filename, task_args->index, task_args->start, task_args->nchunks);
    pthread_exit(reads);
}

void parallel_read(const char* fastq_gz_filename, fastq_gz_index* index, const char* out_filename, uint64_t start, uint64_t nchunks, int nthreads) {
    if (nthreads > nchunks) {
        nthreads = nchunks;
    }
    else if (nthreads >= index->size) {
        printf("Setting num_threads to %llu from %d", index->size - 1, nthreads);
        nthreads = index->size - 1;
    } 
    else if (nthreads > MAXTHREADS) {
        printf("Max number of threads is %d", MAXTHREADS);
        nthreads = MAXTHREADS;
    }

    uint64_t n_per_thread = (int) floor((double) nchunks / nthreads);
    
    pthread_t threads[MAXTHREADS];
    struct task_args args[MAXTHREADS];
    for (uint64_t i = 0; i < nthreads; i++) {
        args[i].start = start + (n_per_thread * i);
        args[i].nchunks = (i == nthreads - 1) ? start + nchunks - args[i].start : n_per_thread;
        args[i].fastq_gz_filename = fastq_gz_filename;
        args[i].index = index;
        pthread_create(&threads[i], NULL, task, &args[i]);
    }

    char* thread_results[MAXTHREADS];
    for (uint64_t i = 0; i < nthreads; i++) {
        pthread_join(threads[i], (void **) &thread_results[i]);
    }

    FILE* fp = fopen(out_filename, "w");
    if (!fp) {
        printf("Failed to open %s for writing\n", out_filename);
        exit(1);
    }

    for (uint64_t i = 0; i < nthreads; i++) {
        fprintf(fp, "%s", thread_results[i]);
        free(thread_results[i]);
    }
}

int main(int argc, char* argv[]) {
    int nthreads = 1;

    if (!(argc == 4 || argc == 5)) {
        printf("Usage: %s <gz> <index> <output> [nthreads]\n", argv[0]);
        exit(1);
    }
    if (argc == 5) {
        nthreads = atoi(argv[4]);
    }

	fastq_gz_index* index = read_index_file(argv[2]);

    time_t start_time = time(NULL);
    parallel_read(argv[1], index, argv[3], 1, index->size - 1, nthreads);
    time_t end_time = time(NULL);
    double elapsed_time = difftime(end_time, start_time);
    printf("Time: %f seconds\n", elapsed_time);

    free(index->index);
    free(index);
	return 0;
}
