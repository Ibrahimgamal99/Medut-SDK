#!/usr/bin/env bash
# Build the JNI layer against the Linux library and run DesktopTest.kt on a
# plain JVM.  Needs scripts/build-linux.sh to have produced
# dist/linux/x86_64/medut.so, a JDK, and a Kotlin compiler: `kotlinc` on PATH,
# or KOTLINC_CP set to a kotlin-compiler-embeddable classpath (the Gradle cache
# has one after any Android build).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../../.." && pwd)"
OUT="${OUT:-${ROOT}/build/kotlin-desktop-test}"
SO="${ROOT}/dist/linux/x86_64/medut.so"

[ -f "${SO}" ] || { echo "error: ${SO} missing; run scripts/build-linux.sh" >&2; exit 1; }
JAVA_HOME="${JAVA_HOME:-$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")}"

rm -rf "${OUT}"
mkdir -p "${OUT}/classes"

echo "=== JNI layer"
cp "${SO}" "${OUT}/medut.so"
gcc -shared -fPIC -O1 -g -std=gnu11 -Wall -Wextra -Werror \
  -I"${JAVA_HOME}/include" -I"${JAVA_HOME}/include/linux" -I"${ROOT}/include" \
  "${ROOT}/bindings/kotlin/medut/src/main/cpp/medut_jni.c" \
  -L"${OUT}" -l:medut.so -Wl,-rpath,'$ORIGIN' -lpthread \
  -o "${OUT}/libmedut_jni.so"

echo "=== Kotlin (core sources only; android/ needs the SDK)"
SRCS=$(find "${ROOT}/bindings/kotlin/medut/src/main/kotlin/dev/medut" -name '*.kt' -not -path '*/android/*')
if command -v kotlinc >/dev/null; then
  kotlinc ${SRCS} "${HERE}/DesktopTest.kt" -jvm-target 17 -d "${OUT}/classes"
else
  : "${KOTLINC_CP:?set KOTLINC_CP or put kotlinc on PATH}"
  STDLIB=$(tr ':' '\n' <<<"${KOTLINC_CP}" | grep '/kotlin-stdlib-[0-9.]*\.jar$')
  java -cp "${KOTLINC_CP}" org.jetbrains.kotlin.cli.jvm.K2JVMCompiler \
    -no-stdlib -no-reflect -classpath "${STDLIB}" -jvm-target 17 \
    -d "${OUT}/classes" ${SRCS} "${HERE}/DesktopTest.kt"
fi

echo "=== Run"
STDLIB=${STDLIB:-$(dirname "$(readlink -f "$(command -v kotlinc)")")/../lib/kotlin-stdlib.jar}
java -Xcheck:jni -Ddev.medut.jniLibrary="${OUT}/libmedut_jni.so" \
  -cp "${OUT}/classes:${STDLIB}" DesktopTestKt
