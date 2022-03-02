#! /bin/ksh -p
#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#

#
# Copyright 2007 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#

#
# Copyright (c) 2013, 2016 by Delphix. All rights reserved.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/snapshot/snapshot.cfg

#
# DESCRIPTION:
# An archive of a zfs dataset and an archive of its snapshot
# changed since the snapshot was taken.
#
# STRATEGY:
# 1) Create some files in a ZFS dataset
# 2) Create a tarball of the dataset
# 3) Create a snapshot of the dataset
# 4) Remove all the files in the original dataset
# 5) Create a tarball of the snapshot
# 6) Extract each tarball and compare directory structures
#

verify_runnable "both"

function cleanup
{
	if [[ -d $CWD ]]; then
		cd $CWD || log_fail "Could not cd $CWD"
	fi

	snapexists $SNAPCTR
	if [[ $? -eq 0 ]]; then
		log_must zfs destroy $SNAPCTR
	fi

	if [[ -e $SNAPDIR1 ]]; then
		log_must rm -rf $SNAPDIR1 > /dev/null 2>&1
	fi

	if [[ -e $TESTDIR1 ]]; then
		log_must rm -rf $TESTDIR1/* > /dev/null 2>&1
	fi

	if [[ -d "$SNAPSHOT_TARDIR" ]]; then
		log_must rm -rf $SNAPSHOT_TARDIR > /dev/null 2>&1
	fi
}

log_assert "Verify that an archive of a dataset is identical to " \
   "an archive of the dataset's snapshot."

SNAPSHOT_TARDIR="$(mktemp -d /tmp/zfstests_snapshot_006.XXXXXX)"
log_onexit cleanup

typeset -i COUNT=21
typeset OP=create

[[ -n $TESTDIR1 ]] && rm -rf $TESTDIR1/* > /dev/null 2>&1

log_note "Create files in the zfs dataset ..."

typeset i=1
while [ $i -lt $COUNT ]; do
	log_must file_write -o $OP -f $TESTDIR1/file$i \
	    -b $BLOCKSZ -c $NUM_WRITES -d $DATA
	(( i = i + 1 ))
done

log_note "Create a tarball from $TESTDIR1 contents..."
CWD=$PWD
cd $TESTDIR1 || log_fail "Could not cd $TESTDIR1"
log_must tar cf $SNAPSHOT_TARDIR/original.tar .
cd $CWD || log_fail "Could not cd $CWD"

log_note "Create a snapshot and mount it..."
log_must zfs snapshot $SNAPCTR

log_note "Remove all of the original files..."
log_must rm -f $TESTDIR1/file* > /dev/null 2>&1

log_note "Create tarball of snapshot..."
CWD=$PWD
cd $SNAPDIR1 || log_fail "Could not cd $SNAPDIR1"
log_must tar cf $SNAPSHOT_TARDIR/snapshot.tar .
cd $CWD || log_fail "Could not cd $CWD"

log_must mkdir $TESTDIR1/original
log_must mkdir $TESTDIR1/snapshot

CWD=$PWD
cd $TESTDIR1/original || log_fail "Could not cd $TESTDIR1/original"
log_must tar xf $SNAPSHOT_TARDIR/original.tar

cd $TESTDIR1/snapshot || log_fail "Could not cd $TESTDIR1/snapshot"
log_must tar xf $SNAPSHOT_TARDIR/snapshot.tar

cd $CWD || log_fail "Could not cd $CWD"

log_must directory_diff $TESTDIR1/original $TESTDIR1/snapshot
log_pass "Directory structures match."
