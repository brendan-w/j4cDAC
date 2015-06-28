j4cDAC
======

The Ether Dream is an extensible laser DAC. It supports Ethernet, USB, microSD, and ILDA, and has a set of headers for future expansion. The firmware, protocol, and driver are all modular and open.

For more information, visit:

http://www.ether-dream.com/



Building
--------

To build this, you will want to have the CodeSourcery ARM toolchain:
- http://www.codesourcery.com/sgpp/lite/arm/portal/release1592

And lpc21isp for programming:
- svn co https://lpc21isp.svn.sourceforge.net/svnroot/lpc21isp lpc21isp


Debug
-----
Debug data can be collected from the DAC using a 5V FTDI adapter such as [this unit](https://www.sparkfun.com/products/9717). To view this output, simply run:

```shell
$ cd firmware/
$ make term
```
