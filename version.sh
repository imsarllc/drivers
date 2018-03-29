#!/bin/sh -e
FILE=${1:-.}/version.h
GIT_DESCRIBE=`git describe --long --dirty --always --tags`
BUILD_DATE=`date +"%F_%T"`

cat <<EOF > ${FILE}
#ifndef _VERSION_H_
#define _VERSION_H_

#define GIT_DESCRIBE  "${GIT_DESCRIBE}"
#define BUILD_DATE    "${BUILD_DATE}"
#endif //_VERSION_H_
EOF

cat ${FILE}
