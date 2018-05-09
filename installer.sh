#!/bin/bash -e

SCRIPT=$(basename $0)
SCRIPT=${SCRIPT%.*}
VERSION=
NODE=`uname -n`
DST=/opt/tools/drivers
ARCHIVE=$(awk '/^__ARCHIVE__/ {print NR + 1; exit 0; }' "${0}")
CHECK=true
KREL=`uname -r`
KDRV=/usr/lib/modules/${KREL}/kernel/drivers/imsar

##############################
# parse arguments
##############################
for ARG in $@; do
  case ${ARG} in
    "-h"|"--help")
      echo "${SCRIPT} ${VERSION}"
      echo "  -v,--version  display this bundle's version"
      echo "  -t,--list     list the contents of the archive"
      echo "  -n,--dry-run  show what would happen, but do nothing"
      echo "  -c,--check    verify image(s) not already installed"
      exit ;;
    "-c"|"--check")
      CHECK=true ;;
    "-v"|"--version")
      echo "${SCRIPT} ${VERSION}"
      exit ;;
    "-t"|"--list")
      tail -n+${ARCHIVE} "${0}" | tar tz | grep -e "[^/]$"
      exit ;;
    "-n"|"--dry-run")
      echo "Items would be placed under:"
      echo " /usr/lib/modules/\${kernel-release}/kernel/drivers/imsar/"
      echo " /etc/udev/rules.d/"
      tail -n+${ARCHIVE} "${0}" | tar tz | grep -e "[^/]$"
      exit ;;
    *)
      echo "ERROR: unrecognized option: ${ARG}"
      exit 1 ;;
  esac
done

##############################
# check
##############################
if ${CHECK}; then
  SCRIPT_NAME=$(basename $0)
  TMP=.${SCRIPT_NAME%.*}
  [ -d ${TMP} ] && rm -rf ${TMP} || :
  mkdir -p ${TMP}
  tail -n+${ARCHIVE} "${0}" | tar xpz -C ${TMP} 2> /dev/null
  MATCH=true

  function check_armhf_drivers {
    local FILES=
    case ${KREL} in
      "3.12"*) FILES=${TMP}/2013.4/*.ko ;;
      *) echo "WARNING: kernel release not supported: ${KREL} (skipping)" ;;
    esac
    for F in ${FILES}; do
      ! cmp --silent ${F} ${KDRV}/${F##*/} && MATCH=false && break || :
    done
    FILES=${TMP}/*.rules
    for F in ${FILES}; do
      ! cmp --silent ${F} /etc/udev/rules.d/${F##*/} && MATCH=false && break || :
    done
  }

  case ${NODE} in
    "microzed"|"grizzly")
      check_armhf_drivers
      ;;
    *)
      echo "ERROR: unrecognized node: ${NODE}"
      echo "expected \"microzed\", \"grizzly\", or \"Vesuvius\""
      exit 1 ;;
  esac
  rm -rf ${TMP}
  ${MATCH} && echo "this version is already installed" && exit || echo "Verification failed.  Installing new version"
fi # CHECK

##############################
# extract
##############################
mkdir -p ${DST}
tail -n+${ARCHIVE} "${0}" | tar xpzv -C ${DST} | grep -e "[^/]$" 2> /dev/null

##############################
# install
##############################
function install_armhf_drivers {
  [ ! -d ${KDRV} ] && mkdir ${KDRV}
  case ${KREL} in
    "3.12"*) cp ${DST}/2013.4/*.ko ${KDRV} ;;
    *) echo "WARNING: kernel release not supported: ${KREL} (skipping)" ;;
  esac
  cp ${DST}/*.rules /etc/udev/rules.d/
  depmod -a
  rm -rf ${DST}
}

case ${NODE} in
  "microzed"|"grizzly")
    install_armhf_drivers
    ;;
  *)
    echo "ERROR: unrecognized node: ${NODE}"
    echo "expected \"microzed\", \"grizzly\", or \"Vesuvius\""
    exit 1 ;;
esac

exit 0

__ARCHIVE__
