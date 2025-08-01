#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

echo "Boot config test script"

if [ -d "$1" ]; then
  TESTDIR=$1
else
  TESTDIR=.
fi
BOOTCONF=${TESTDIR}/bootconfig
ALIGN=4

INITRD=`mktemp ${TESTDIR}/initrd-XXXX`
TEMPCONF=`mktemp ${TESTDIR}/temp-XXXX.bconf`
OUTFILE=`mktemp ${TESTDIR}/tempout-XXXX`
NG=0

cleanup() {
  rm -f $INITRD $TEMPCONF $OUTFILE
  exit $NG
}

trap cleanup EXIT TERM

NO=1

xpass() { # pass test command
  echo "test case $NO ($*)... "
  if ! ($@ && printf "\t\t[OK]\n"); then
     printf "\t\t[NG]\n"; NG=$((NG + 1))
  fi
  NO=$((NO + 1))
}

xfail() { # fail test command
  echo "test case $NO ($*)... "
  if ! (! $@ && printf "\t\t[OK]\n"); then
     printf "\t\t[NG]\n"; NG=$((NG + 1))
  fi
  NO=$((NO + 1))
}

echo "Basic command test"
xpass $BOOTCONF $INITRD

echo "Delete command should success without bootconfig"
xpass $BOOTCONF -d $INITRD

dd if=/dev/zero of=$INITRD bs=4096 count=1
printf "key = value;" > $TEMPCONF
bconf_size=$(wc -c < $TEMPCONF)
initrd_size=$(wc -c < $INITRD)

echo "Apply command test"
xpass $BOOTCONF -a $TEMPCONF $INITRD
new_size=$(wc -c < $INITRD)

echo "Show command test"
xpass $BOOTCONF $INITRD

echo "File size check"
total_size=$(expr $bconf_size + $initrd_size + 9 + 12 + $ALIGN - 1 )
total_size=$(expr $total_size / $ALIGN)
total_size=$(expr $total_size \* $ALIGN)
xpass test $new_size -eq $total_size

echo "Apply command repeat test"
xpass $BOOTCONF -a $TEMPCONF $INITRD

echo "File size check"
xpass test $new_size -eq $(wc -c < $INITRD)

echo "Delete command check"
xpass $BOOTCONF -d $INITRD

echo "File size check"
new_size=$(wc -c < $INITRD)
xpass test $new_size -eq $initrd_size

echo "No error messge while applying"
dd if=/dev/zero of=$INITRD bs=4096 count=1
printf " \0\0\0 \0\0\0" >> $INITRD
$BOOTCONF -a $TEMPCONF $INITRD > $OUTFILE 2>&1
xfail grep -i "failed" $OUTFILE
xfail grep -i "error" $OUTFILE

echo "Max node number check"

awk '
BEGIN {
  for (i = 0; i < 26; i += 1)
      printf("%c\n", 65 + i % 26)
  for (i = 26; i < 8192; i += 1)
      printf("%c%c%c\n", 65 + i % 26, 65 + (i / 26) % 26, 65 + (i / 26 / 26))
}
' > $TEMPCONF
xpass $BOOTCONF -a $TEMPCONF $INITRD

printf "badnode\n" >> $TEMPCONF
xfail $BOOTCONF -a $TEMPCONF $INITRD

echo "Max filesize check"

# Max size is 32767 (including terminal byte)
printf "data = \"" > $TEMPCONF
dd if=/dev/urandom bs=768 count=32 | base64 -w0 >> $TEMPCONF
printf "\"\n" >> $TEMPCONF
xfail $BOOTCONF -a $TEMPCONF $INITRD

dd if=$TEMPCONF of=$OUTFILE bs=1 count=32764
cp $OUTFILE $TEMPCONF
printf "\"\n" >> $TEMPCONF	# add 2 bytes + terminal ('\"\n\0')
xpass $BOOTCONF -a $TEMPCONF $INITRD

echo "Adding same-key values"
cat > $TEMPCONF << EOF
key = bar, baz
key += qux
EOF
echo > $INITRD

xpass $BOOTCONF -a $TEMPCONF $INITRD
$BOOTCONF $INITRD > $OUTFILE
xpass grep -q "bar" $OUTFILE
xpass grep -q "baz" $OUTFILE
xpass grep -q "qux" $OUTFILE

echo "Override same-key values"
cat > $TEMPCONF << EOF
key = bar, baz
key := qux
EOF
echo > $INITRD

xpass $BOOTCONF -a $TEMPCONF $INITRD
$BOOTCONF $INITRD > $OUTFILE
xfail grep -q "bar" $OUTFILE
xfail grep -q "baz" $OUTFILE
xpass grep -q "qux" $OUTFILE

echo "Double/single quotes test"
printf "key = '\"string\"';" > $TEMPCONF
$BOOTCONF -a $TEMPCONF $INITRD
$BOOTCONF $INITRD > $TEMPCONF
cat $TEMPCONF
xpass grep \'\"string\"\' $TEMPCONF

echo "Repeat same-key tree"
cat > $TEMPCONF << EOF
foo
bar
foo { buz }
EOF
echo > $INITRD

xpass $BOOTCONF -a $TEMPCONF $INITRD
$BOOTCONF $INITRD > $OUTFILE
xpass grep -q "bar" $OUTFILE


echo "Remove/keep tailing spaces"
cat > $TEMPCONF << EOF
foo = val     # comment
bar = "val2 " # comment
EOF
echo > $INITRD

xpass $BOOTCONF -a $TEMPCONF $INITRD
$BOOTCONF $INITRD > $OUTFILE
xfail grep -q 'val[[:space:]]' $OUTFILE
xpass grep -q 'val2[[:space:]]' $OUTFILE

echo "=== expected failure cases ==="
for i in samples/bad-* ; do
  xfail $BOOTCONF -a $i $INITRD
done

echo "=== expected success cases ==="
for i in samples/good-* ; do
  xpass $BOOTCONF -a $i $INITRD
done


echo
echo "=== Summary ==="
echo "# of Passed: $(expr $NO - $NG - 1)"
echo "# of Failed: $NG"

echo
if [ $NG -eq 0 ]; then
	echo "All tests passed"
else
	echo "$NG tests failed"
	exit 1
fi
