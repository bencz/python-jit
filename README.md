This project is completely based on the work done by: fuzziqersoftware

ORIGINAL PROJECT
================
* https://github.com/fuzziqersoftware/nemesys
* https://github.com/fuzziqersoftware/phosg
* https://github.com/fuzziqersoftware/libamd64

Build
===========================
* This project runs ONLY on Linux... Tested on Ubuntu and Debian.
```
git clone --recursive https://github.com/bencz/python-jit
cd python-jit
mkdir build
cd build 
cmake ..
make -j 4
```

Main changes that were made
===========================
* Changed from makefile to CMake
* Fixed some code bugs
* Removed dependency with phosg and libamd64 ( it's not necessary to install it on the system anymore )
* * The phosg and libamd64 code is now integrated into the compiler code
* Fixed global variable `__file__` to make it compatible with python3

