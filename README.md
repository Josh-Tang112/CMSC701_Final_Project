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
