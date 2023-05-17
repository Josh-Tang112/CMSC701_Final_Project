Both the ```convert.c``` and ```read.c``` programs for building the sync point index and
parallel parsing a file, respectively, can be build by running ```make```. This should
result in the executables ```convert``` and ```read```.

Running the ```convert``` program is as follows:

./convert <input gz> <output gz> <idx> [reads per chunk]

where <input gz> represents the gzip file to be recompressed with sync points, 
<output gz> is the name of the new gzip file, and <idx> is the name of the
index file to be created. [reads per chunk] is an optional argument that denotes
how many reads must be seen before a sync point is inserted. By default, this
argument is 10,000.

Running the ```read``` program is as follows:

./read <input gz> <idx> <output> [nthreads]

where <input gz> is the name of a gzip file that has been compressed with
sync points, <idx> is the name of the index file corresponding to the input
gzip, and <output> is the name of the file that the decompressed data will
be written to. [nthreads] is the number of threads to use during parsing.
By default, the parser is single-threaded. The maximum number of threads
that can be specified is 16, though this can be modified by changing the
MAXTHREADS macro.
