#!/bin/bash
#
# Copyright (c) 1994-2012 Carnegie Mellon University.  All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
#
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in
#    the documentation and/or other materials provided with the
#    distribution.
#
# 3. The name "Carnegie Mellon University" must not be used to
#    endorse or promote products derived from this software without
#    prior written permission. For permission or any legal
#    details, please contact
#      Carnegie Mellon University
#      Center for Technology Transfer and Enterprise Creation
#      4615 Forbes Avenue
#      Suite 302
#      Pittsburgh, PA  15213
#      (412) 268-7393, fax: (412) 268-7395
#      innovation@andrew.cmu.edu
#
# 4. Redistributions of any form whatsoever must retain the following
#    acknowledgment:
#    "This product includes software developed by Computing Services
#     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
#
# CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
# THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
# AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
# FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
# AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
# OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
#
#
# Script to configure and build Cyrus from a new git checkout,
# and run some tests.  Designed to be used from the Jenkins CI
# server as a build script.
#

function fatal()
{
    echo "$0: $*" 1>&2
    exit 1
}

echo "==================== CYRUS IMAPD ===================="
if [ "$1" == --manual ] ; then
    echo "Invoked manually"
    BUILD_ID=build$(date +%Y%m%dT%H%M%S)
    WORKSPACE=/home/gnb/software/cyrus
    COVERAGE=coverage_
    TGGCOV=/home/gnb/software/ggcov/src/tggcov
    HISTCOV=/home/gnb/software/ggcov/scripts/git-history-coverage
else
    echo "Invoked from Jenkins"
    [ -n "$JENKINS_HOME" ] || fatal "No \$JENKINS_HOME defined in environment"
    [ -n "$BUILD_ID" ] || fatal "Did not receive \$BUILD_ID in environment from Jenkins"
    [ -n "$WORKSPACE" ] || fatal "Did not receive \$WORKSPACE in environment from Jenkins"
    COVERAGE=coverage_
    TGGCOV=tggcov
    HISTCOV=git-history-coverage

    echo "Workspace is $WORKSPACE"
    if [ -n "$COVERAGE" ]; then
	echo "Coverage is enabled"
    else
	echo "Coverage is disabled"
    fi

    # We want new files to be group-writable
    # so that the Cassandane tests running
    # as user 'cyrus' can write them
    umask 002
fi

## Ensure $PATH is right
#PATH=/usr/bin:/bin:/usr/sbin:$PATH

CYRUS_SRC="$WORKSPACE/imapd"
CYRUS_INST="$WORKSPACE/inst"
CASSANDANE_SRC="$WORKSPACE/cassandane"
COPTIMISEFLAGS="-O0"
CONFIGURE_ARGS="\
    --prefix=/usr/cyrus \
    --with-cyrus-prefix=/usr/cyrus \
    --with-bdb=4.8 \
    --with-openssl \
    --enable-sieve \
    --enable-idled \
    --enable-nntp \
    --enable-murder \
    --enable-replication \
    --enable-unit-tests \
    "

NCPUS=$(grep '^processor' /proc/cpuinfo | wc -l)
[ $NCPUS -ge 1 ] || fatal "Can't get number of CPUs"

[ -d "$CYRUS_SRC" ] || fatal "$CYRUS_SRC: no such directory"
cd "$CYRUS_SRC" || fatal "$CYRUS_SRC: cannot cd"
[ -d .git ] || fatal "$CYRUS_SRC: not a git repository"
nfiles=$(git ls-files|wc -l)
[ $nfiles -gt 0 ] || fatal "$CYRUS_SRC: cannot list git controlled files"

BRANCH=$(git branch | sed -n -e 's/^\*[ \t]\+//p')
[ -n "$BRANCH" ] || fatal "Can't get git branch"
[ "$BRANCH" != "(no branch)" ] || fatal "Not on any git branch"
COMMIT=$(git log --pretty='format:%h' HEAD^..HEAD|head -1)
[ -n "$COMMIT" ] || fatal "Can't get git top commit"
echo "Building on git branch $BRANCH, top commit $COMMIT"
CONFIGURE_ARGS="--with-extraident=$BRANCH-$COMMIT $CONFIGURE_ARGS"

set -x
git ls-files -o
git status


# do the whole autotools dance
aclocal -I cmulocal || fatal "Can't run aclocal"
autoconf || fatal "Can't run autoconf"
autoheader || fatal "Can't run autoheader"
[ -f configure ] || fatal "autoconf did not produce a configure script"
./configure $CONFIGURE_ARGS || fatal "Cannot run configure"
[ -f config.status ] || fatal "configure did not produce a config.status script"
[ -f lib/Makefile ] || fatal "configure did not produce lib/Makefile"
[ -f imap/Makefile ] || fatal "configure did not produce a imap/Makefile"

# Tweak makefiles for optimisation flags
perl -p -i.orig -e 's/^(CFLAGS\s*=\s*.*)\s+-O2/\1 '"$COPTIMISEFLAGS"'/' $mf $(find . -name Makefile)

# hack to work around a makefile race condition
# make -C sieve sieve.c sieve.h addr.c addr.h
# Finally the actual build
# make -j$NCPUS ${COVERAGE}all || fatal "Can't make all"
make ${COVERAGE}all || fatal "Can't make all"

# Run CUnit based unit tests
# [ -n "$COVERAGE" ] && find . -name '*.gcda' -exec rm '{}' \;
make CUFORMAT=junit ${COVERAGE}check || fatal "Can't make check"

# Do a temporary install for Cassandane
[ -d $CYRUS_INST.old ] && rm -rf $CYRUS_INST.old
[ -d $CYRUS_INST ] && mv -f $CYRUS_INST $CYRUS_INST.old
mkdir -p $CYRUS_INST || fatal "$CYRUS_INST: can't mkdir"
make DESTDIR=$CYRUS_INST install || fatal "Can't install"

exitcode=0

# Run Cassandane tests
if [ -d $CASSANDANE_SRC ]; then

## Not needed anymore, user cyrus is in group tomcat
#     if [ -n "$COVERAGE" ]; then
# 	chmod 666 $(find . -type f -name '*.gcda')
#     fi

    # TODO: factor this out into a shell function
    cd "$CASSANDANE_SRC" || fatal "$CASSANDANE_SRC: cannot cd"
    [ -d .git ] || fatal "$CASSANDANE_SRC: not a git repository"
    nfiles=$(git ls-files|wc -l)
    [ $nfiles -gt 0 ] || fatal "$CASSANDANE_SRC: cannot list git controlled files"

    git ls-files -o
    git status

    make || fatal "Can't make in cassandane/";

    # Build cassandane.ini
    sed -e 's|^##destdir =.*$|destdir = '$CYRUS_INST'|' \
        -e 's|^##pwcheck = .*$|pwcheck = sasldb|' \
	< cassandane.ini.example \
	> cassandane.ini

    rm -rf reports.old
    mv -f reports reports.old
    mkdir -m 0777 reports || fatal "Can't mkdir reports"

    ./testrunner.pl --cleanup -f xml -v > cass.errs 2>&1 || exitcode=1
fi

# Report on coverage
# [ -n "$COVERAGE" ] && $TGGCOV --report=summary_all -r .  2>/dev/null
[ -n "$COVERAGE" ] && $TGGCOV --report=cobertura -r . 2>/dev/null > coverage.xml

# The first line in this file is like
# Changes in branch origin/for-upstream, between $SHA and $SHA
if [ -n "$JENKINS_HOME" ] ; then
revlist=$(perl \
    -n \
    -e 's/^Changes .*between ([[:xdigit:]]{40}) and ([[:xdigit:]]{40})/\1..\2/; print; exit 0;' \
    $WORKSPACE/../builds/$BUILD_ID/changelog.xml \
    2>/dev/null)
[ -n "$revlist" ] && $HISTCOV $revlist 2>/dev/null
fi

exit $exitcode