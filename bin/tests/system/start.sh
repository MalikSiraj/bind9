#!/bin/sh
#
# Copyright (C) 2000  Internet Software Consortium.
# 
# Permission to use, copy, modify, and distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
# 
# THE SOFTWARE IS PROVIDED "AS IS" AND INTERNET SOFTWARE CONSORTIUM DISCLAIMS
# ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES
# OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL INTERNET SOFTWARE
# CONSORTIUM BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
# DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
# PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
# ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
# SOFTWARE.

#
# Start name servers for running system tests.
#

SYSTEMTESTTOP=.
. $SYSTEMTESTTOP/conf.sh

cd $1

for d in ns*
do
    (
        cd $d
	rm -f *.jnl *.bk named.run &&
	if test -f named.pid
	then
	    if kill -0 `cat named.pid` 2>/dev/null
	    then
		echo "$0: named pid `cat named.pid` still running" >&2
	        exit 1
	    else
		rm -f named.pid
	    fi
	fi
	$NAMED -c named.conf -d 99 -g >named.run 2>&1 &
	while test ! -f named.pid
	do
	    sleep 1
        done
    )
done


# Make sure all of the servers are up.

status=0

for d in ns*
do
	n=`echo $d | sed 's/ns//'`
	$DIG +tcp +noadd +nosea +nostat +noquest +nocomm +nocmd \
		version.bind. chaos txt @10.53.0.$n soa > dig.out
	status=`expr $status + $?`
	grep ";" dig.out
done
rm -f dig.out

exit $status
