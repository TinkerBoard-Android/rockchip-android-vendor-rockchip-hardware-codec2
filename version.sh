#!/bin/bash
CURRENTDIR=`dirname $0`
cd $CURRENTDIR

VERSION=`git log \-1 | grep commit | head -n 1`
CURRENT_TIME=`date +"%Y-%m-%d %H:%M:%S"`
PRODUCT=$TARGET_PRODUCT
VERSION_TARGET="$(cat version.h.template | sed  -e 's/\$GIT_BUILD_VERSION/'"$VERSION build: $CURRENT_TIME running on $PRODUCT"'/g' version.h.template)"

echo "${VERSION_TARGET}"
