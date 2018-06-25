
How to build jemalloc for Windows
=================================

1. Install Cygwin with at least the following packages:
   * autoconf
   * autogen
   * gawk
   * grep
   * sed

2. Install Visual Studio 2015 with Visual C++

3. Add Cygwin\bin to the PATH environment variable

4. Open "VS2015 x86 Native Tools Command Prompt"
   (note: x86/x64 doesn't matter at this point)

5. Generate header files:
   sh -c "CC=cl ./autogen.sh"

6. Now the project can be opened and built in Visual Studio:
   msvc\jemalloc_vc2015.sln

