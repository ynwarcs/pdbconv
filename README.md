
**pdbconv** is a converter for PDB files. It makes it possible to convert native PDB files between their regular MSF format and the new MSFZ format that supports compression. For context, you can read [my blog post](https://ynwarcs.github.io/pdbconv-pdb-compression) on the new format.

### building & running
- open **pdbconv.sln** in Visual Studio 2022
- build it
- run **pdbconv.exe**

### usage
```
Usage: pdbcpy [args]
Arguments:
(-b) --block_size={value} (default 4096) | Block size value to use for the output MSF streams when using --decompress.
(-c) --compress | Compress input PDB file to a MSFZ format output file.
(-x) --decompress | Decompress input file in the MSFZ format to a regular PDB output file.
(-f) --fragment_size={value} (default 4096) | Fixed fragment size value to use when using --compress and --strategy=MultiFragment.
(-i) --input={value} | Path to the input file when using --compress or --decompress or the input directory when using --test.
(-l) --level={value} (1-22, default 3) | ZSTD compression level to use when using --compress..
(-m) --max_frps={value} (default 4096) | Maximum number of fragments per stream when using --compress and --strategy=MultiFragment.
(-o) --output={value} | Path to the output file when using --compress or --decompress or the output directory when using --test.
(-s) --strategy={value} (NoCompression, SingleFragment, MultiFragment) | Compression strategy to use when using --compress.
(-t) --test | Run test batch conversion on directory.
--thread_num={value}(default 75% of processor count) | Number of threads to use for compression or decompression workflows.
```

#### compression
Of course, the most interesting feature of the program is the ability to create PDB files in this new format. We can run compression by specifying **-\-compress** (or **-c**) and providing arguments:
- **-\-input** and **-\-output** for the input file we wish to convert and the output file that we want to be our result. The input file must be a valid MSF-format PDB file.
- **-\-strategy**={NoCompression, SingleFragment, MultiFragment} for the compression strategy we wish to employ. I discuss these strategies below.
- **-\-level**, the compression level to be used for compression. This value has the same meaning as the `compressionLevel` parameter in `zstd_compress` function that's used to compress data (ref. [zstd manual](http://facebook.github.io/zstd/zstd_manual.html)).
- (optional) **-\-fixed_fragment_size**, if we want to fix the size of each fragment for each stream. This argument should only be used when strategy is set to **MultiFragment**, as it doesn't make sense otherwise.
- (optional) **-\-max_frps**, if we want to limit the number of fragments that any single stream can have. This argument should also only be used when strategy is set to **MultiFragment**, as it doesn't make sense otherwise.

The strategies are fairly simple:
- **NoCompression**  will not compress any data. This basically sets `m_IsCompressed` field in each `MsfzFragment` object to false and doesn't compress the data in chunks, leaving it in its raw form. Not very useful in the real world, but works as a reference point for benchmarks. Interestingly, even using this method we average a 90% compression ratio, just based on memory waste of MSF.
- **SingleFragment** will serialize each stream as a single fragment. Each fragment will have its own chunk, and each chunk will be compressed. This method achieves the best compression ratio, but this also means that each stream will be fully decompressed at runtime once some data from it is needed. This could impact the performance & memory usage of the user significantly.
- **MultiFragment** will serialize each stream in multiple fragments. Similarly to the previous strategy, we'll use a one fragment -> one chunk mapping, as I didn't find a good use for having a more complex pairing here. The extra arguments here are **-\-fixed_fragment_size** and **-\-max_frps**, which control how we'll split the streams into fragments. It's worth noting that **max_frps** will always override **fixed_fragment_size**, if using the latter would mean that the number of fragments for the stream would surpass the limit imposed by the former. For example, if we set **fixed_fragment_size=0x1000** and **max_frps=0x10**, when serializing a stream that has `0x18000` bytes the program will create `0x10` fragments of size `0x1800`, rather than `0x18` fragments of size `0x1000`.

#### decompression
Decompression is basically just the reverse conversion (MSFZ -> MSF). We run it by specifying **-\-decompress** and providing arguments:
- **-\-input** and **-\-output** for the input file we wish to convert and the output file that we want to be our result. The input file must be a valid MSFZ PDB file.
- **-\-block_size**, the block size of the output MSF file. It must be a power of two between `0x200` and `0x2000`. There are also some internal limits, e.g. we won't be able to produce a PDB file whose size surpasses `(1 << 20) * block_size` bytes.

#### tests
I don't recommend running tests unless you're trying to modify something in the code. If you really do want to do it, keep in mind that running the tests will eat your disk space + take a very long time. Tests can be run via `run_tests.bat` script in the `scripts` folder. It takes two arguments - the first being a directory containing input PDB files (MSF format) that are going to be used for tests, and the second being an output directory that's going to be used for converted PDB files. All of the output converted files will take about 70x the size of the input file in total, so make sure you have enough space. The tests make use of the [`Dia2Dump`](https://learn.microsoft.com/en-us/visualstudio/debugger/debug-interface-access/dia2dump-sample?view=vs-2022) program to dump data in the PDB file, make sure you compile it (VS2022 - Release - x64) before running the tests, or modify the path to the executable in the script to the one that you're using.

#### notes
- Both compression & decompression are multi-threaded. You can control the thread count with the **-\-thread_num** argument. By default, it will use 75% use of the available cores (usually with 2 threads per core, this translates to 37.5% CPU usage).
- If using  the **MultiFragment** strategy on large PDBs, there may be a pretty big slowdown if a stream has too many fragments. Certain streams call `GetCbStream()` function quite often, which is meant to return the length of the entire stream. In the MSFZ format, this function has to walk through the entire list of fragments and add up the sizes. This causes some rather heavy slowdowns in certain situations. I imagine this is something that MS will correct as they ship the format in the future, either by caching the size once calculated, or letting the format serialize the size as well (in which case they'll break compatibility for pdbconv but I don't mind :<).
- Keep in mind that you need msdia140.dll shipped with at least VS 2022 17.10.0 to be able to parse MSFZ PDBs. Also keep in mind that the format is completely unofficial and MS can change it at will without telling a soul :). In case something breaks, I'll try to stay on top of it, but it's very possible that something may irreparably break in the future. After all, this may have just been a test that mistakenly got shipped (though I doubt it).

#### benchmarks
Below are some compression ratio benchmarks using different parameters. The results were averaged over 20-ish PDBs ranging from 100Kb to 3Gb in size. All tests were done with compression level being set to 3. I didn't want to benchmark this parameter, as it's fully external and should scale based on existing zstd benchmarks.

|    Strategy    | Fragment size | Max frps | Avg compression ratio |
|:--------------:|:-------------:|:--------:|:---------------------:|
|  NoCompression |      N/A      |    N/A   |         89.09%        |
| SingleFragment |      N/A      |    N/A   |         19.46%        |
|  MultiFragment |      256      |     2    |         19.83%        |
|  MultiFragment |      256      |    256   |         32.9%         |
|  MultiFragment |      256      |   12289  |         56.31%        |
|  MultiFragment |      4096     |    256   |         24.5%         |
|  MultiFragment |      4096     |   12289  |         27.4%         |
|  MultiFragment |    1048576    |    256   |         19.63%        |

As expected, setting max frps to 2 produces similar results as having set it to 1 (which is essentially the SingleFragment strategy). As we increase the value of the parameter, the compression ratio increases too. On the other hand, increasing the fragment size parameter decreases the compression ratio, as we compress more data at once when our fragments are larger.

### thanks
This project makes use of [zstd](https://github.com/facebook/zstd). Manual PDB file parsing I implemented was based on the implementation of [raw_pdb](https://github.com/MolecularMatters/raw_pdb). LLVM's [PDB serialization code](https://github.com/llvm-mirror/llvm/tree/2c4ca6832fa6b306ee6a7010bfb80a3f2596f824/lib/DebugInfo/PDB) helped with writing my own.