#!/bin/bash
# Agent REQUIRES EXPLICIT PERMISSION TO EDIT THIS FILE
#
TMP=/tmp/rock$$
TESTS=./test
COMPILER=./rock
testcnt=0
totalpass=0;
totalfail=0;
totalabort=0;

fatal() {
  echo "❌ GENERAL FAILURE $*"
  exit 1
}

cleanFile() {
  f="$1"
  rm -f $TESTS/${f}.exe
  rm -f $TESTS/${f}.exe.c
  rm -rf $TESTS/${f}.exe.dSYM
}

clean() {
  rm -f $TESTS/*.exe
  rm -f $TESTS/*.exe.c
  rm -rf $TESTS/*.dSYM
}

getTests() {
  if [ "$1" != "" ]; then
    ls -1 "$1" || fatal "file $1 not found"
  else
    ls -1 $TESTS/*.rkr | grep -v "Assert.rkr" | grep -v "Mod.rkr"
  fi
}

report() {
  failed=$(expr $(echo "$gResult" | grep "FAIL: " | wc -l ) )
  passed=$(expr $(echo "$gResult" | grep "PASS: " | wc -l ) )
  let testcnt=$testcnt+$passed
  let testcnt=$testcnt+$failed
  let passcnt=$testcnt+$passed
  let totalpass=$totalpass+$passed
  let totalfail=$totalfail+$failed
  if [ $failed -eq 0 ]; then
    echo "✅ $gName. PASSED ALL $passed tests"
  else
    echo "❌ $gName. FAILED:$failed PASSED:$passed"
  fi
}

clean
if [ ! -x $COMPILER ]; then
  fatal "cannot find $COMPILER executable in current directory"
fi


gResult=""
gName=""
FILES=$( getTests "$1" )
for test in $FILES; do
  gName="$(basename $test | cut -f1 -d'.')"

  # compile it...
  EXE="$TESTS/${gName}.exe"
  echo "$COMPILER $test $EXE 2>&1 >$TMP"
  $COMPILER $test $EXE 2>&1 >$TMP
  if [ $? -ne 0 ]; then
    # its a fail if it doesnt compile
    let totalabort=$totalabort+1
    echo "❌ COMPILE ERRORS FOR $test "
    cat $TMP
  else
    # run the compiled program
    ${EXE} 2>&1 >$TMP
    if [ $? -ne 0 ]; then
      echo "❌ EXECUTABLE WILL NOT RUN FOR TEST $test "
      let totalabort=$totalabort+1
      cat $TMP
    else
      # the test ran.  Check it
      gResult=$( cat $TMP )
      if echo "$gResult" | grep -q "error:"; then
        # its a fail if the test signals a failure
        echo "❌ $gName"
        echo "   File: $output.c"
        echo "$gResult" | grep "error:"
        echo ""
      else 
        if echo "$gResult" | grep -q "FAIL: "; then
          report
        else
          report
          cleanFile "${gName}"
        fi # check test run result
      fi
    fi # check test ran
  fi # check test compiled
done

echo "-----------------------------------------------------"
echo "Total tests passed: $totalpass  failed: $totalfail  fatal-errors: $totalabort"
echo

rm -f $TMP
