======================================
Xilinx Zlib Streaming 16KB Compression
======================================

Zlib example resides in ``L2/tests/zlibc_16KB`` directory. 

Follow build instructions to generate host executable and binary.

The binary host file generated is named as "**xil_zlib**" and it is present in ``./build`` directory.

Executable Usage
----------------

1. To execute single file for compression 	    : ``./build_dir.<TARGET mode>.<xsa_name>/xil_zlib -xbin ./build_dir.<TARGET mode>.<xsa_name>/compress.xclbin -c <file_name> -zlib 1``
2. To execute multiple files for compression    : ``./build_dir.<TARGET mode>.<xsa_name>/xil_zlib -xbin ./build_dir.<TARGET mode>.<xsa_name>/compress.xclbin -cfl <files.list> -zlib 1``

	- ``<files.list>``: Contains various file names with current path

The usage of the generated executable is as follows:

.. code-block:: bash
 
   Usage: application.exe -[-h-c-l-xbin-B]
          --help,           -h      Print Help Options
          --xclbin,         -xbin   XCLBIN                                               Default: [compress]
          --compress,       -c      Compress
          --file_list,      -cfl    Compress List of Input Files
          --max_cr,         -mcr    Maximum CR    
          --device_id,      -id     Device ID                                       Default: [0]
          --zlib,           -zlib   [0:GZip, 1:Zlib]                                Default: [0]

Results
-------

Resource Utilization 
~~~~~~~~~~~~~~~~~~~~~

Table below presents resource utilization of Xilinx Zlib Compress/Decompress
kernels. The final Fmax achieved is 300MHz.


========== ===== ====== ===== ===== ===== 
Flow       LUT   LUTMem REG   BRAM  URAM 
========== ===== ====== ===== ===== ===== 
Compress   58K   8.7K   52.9K 160   48    
========== ===== ====== ===== ===== ===== 

Performance Data
~~~~~~~~~~~~~~~~

Table below presents kernel throughput achieved for a single compute
unit. 

============================= =========================
Topic                         Results
============================= =========================
Compression Throughput        2 GB/s
Average Compression Ratio     2.62x (Silesia Benchmark)
============================= =========================

Standard GZip Support
---------------------

This application is compatible with standard Gzip/Zlib application (compress/decompress).  
