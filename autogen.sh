#!/bin/sh

if test "$*"; then
	ARGS="$*"
else
	test -f config.log && ARGS=`grep '^  \$ \./configure ' config.log | sed 's/^  \$ \.\/configure //' 2> /dev/null`
fi

echo "Running aclocal..."
aclocal -I m4 || exit 1

echo "Running autoheader..."
autoheader || exit 1 

echo "Running libtoolize..."
libtoolize --ltdl || exit 1 

echo "Running automake..."
automake --foreign --add-missing 

echo "Running once again aclocal..."
aclocal -I m4 || exit 1 

echo "Running autoconf..."
autoconf || exit 1

test x$NOCONFIGURE = x && echo "Running ./configure $ARGS" && ./configure $ARGS

