#!/bin/sh
#############################################################
# MAKE_TML.sh                                               #
# ......................................................... #
# This script generates Timonel bootloader images that are  #
# flashable with an AVR programmer (e.g. USBasp).           #
#                                                           #
# It's a bit messy, but it gets smaller images by making it #
# with Pass-1 to generate crt1.o, making it again with      #
# Pass-2 to create a .elf file and finally making it once   #
# more with Pass-3 to place it in an upper flash location.  #
# It's needless to say that writing Makefiles is not my     #
# strength, so any help to improve this is very welcome.    #
#                                                           #
# ......................................................... #
# 2018-09-16 Gustavo Casanova                               #
# ......................................................... #
# From Linux: it runs directly.                             #
# From Windows/Mac: you need to install the Git Bash tool   #
# for your system (git-scm.com/downloads).                  #
#                                                           #
#############################################################
echo ""
echo "##########################"
echo "#   >>> TML PASS 1 <<<   #"
echo "##########################"
cp tml-t85-std/Pass-1 Makefile
make clean
make all
echo ""
echo "##########################"
echo "#   >>> TML PASS 2 <<<   #"
echo "##########################"
echo ""
cp tml-t85-std/Pass-2 Makefile
make clean
make all
echo ""
echo "##########################"
echo "#   >>> TML PASS 3 <<<   #"
echo "##########################"
cp tml-t85-std/Pass-3 Makefile
make clean
make all
cp timonel.hex releases
make clean_all
cp tml-t85-std/Pass-1 Makefile