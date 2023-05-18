# CMSC701 Final Project 

## Building and running `index-builder`, `index-reader`, and `base-counter`

### Dependencies

I believe that `zlib1g-dev` is a dependency in order to use `zlib`; I didn't end
up needing to install anything new to compile this project.

### Build

Build `index-builder`, `index-reader`, and `base-counter` using `Make`:

```bash
make
```

### Running `index-builder`

Help is always available:

```bash
> ./index-builder -h                                                            
index-builder builds an index into a gzipped FASTQ file to allow for parallel processing

Usage: ./index-builder [-c CHUNKSIZE] [-o OUTFILE] GZIP_FILE
-c CHUNKSIZE	the integer chunk size with which to store indexes into the gzip file (default 10000)
-o OUTFILE	the name of the output index file to write (default 'output.idx')
-v		enable verbose logging
GZIP_FILE	<gzip file> is a gzipped FASTQ file to index
```

To build an index over a gzip file with a sequence chunksize of 10,000 writing
to output files prefixed with `foo`:

```
./index-builder <fastq.gz> -o foo
```

### Running `index-reader`

To run `index-reader` to have it write out the decompressed FASTQ file,

```bash
./index-reader foo.idx foo.seq-idx <fastq.gz>
```

### Running `base-counter`

To run `base-counter` to have it count nucleotides from the decompressed FASTQ file,

```bash
./base-counter foo.idx foo.seq-idx <fastq.gz>
```



# Probably useful notes

The zlib author discusses how to build an index over a gzipped file to allow for
random reads internal to the file (i.e. without decompressing the whole thing)
[here](https://github.com/madler/zlib/blob/master/examples/zran.c). Namely, the
`deflate_index_build()` function and comments therein seem pretty useful to our
project; essentially we just need to graft information from the genome into the
index he creates, I think.

# Downloading FASTQ files

Instructions pieced together from for instance [this tutorial](https://www.biostars.org/p/111040/)
and [this one too](https://erilu.github.io/python-fastq-downloader/).

On Ubuntu/Debian, install `sra-tools`

```bash
sudo apt install sra-toolkit 
```

Download the sequence data you want,

```bash
 prefetch -v SRR925811
```

This should download the data to a directory named `SRR925811`. Then, convert to
FASTQ format:

```bash
fastq-dump --outdir fastq --gzip --skip-technical  --readids \
--read-filter pass  --dumpbase --split-3 --clip ~/SRR925811/SRR925811.sra
```

will create 2 FASTQ files from this inside of the `~/SRR925811/fastq/` directory.
