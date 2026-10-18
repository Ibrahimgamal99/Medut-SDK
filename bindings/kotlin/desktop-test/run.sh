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
if ! command -v kotlinc >/dev/null && [ -z "${KOTLINC_CP:-}" ]; then
  # The compiler the Android build downloaded, at the Kotlin plugin's version.
  KV=$(sed -n 's/.*org.jetbrains.kotlin.android") version "\([^"]*\)".*/\1/p' "${HERE}/../settings.gradle.kts")
  C="${GRADLE_USER_HOME:-${HOME}/.gradle}/caches/modules-2/files-2.1"
  pick() { find "${C}/$1" -name "$2" -not -name '*-sources.jar' 2>/dev/null | sort -V | tail -1; }
  KOTLINC_CP=""
  for j in "$(pick org.jetbrains.kotlin/kotlin-compiler-embeddable "kotlin-compiler-embeddable-${KV}.jar")" \
           "$(pick org.jetbrains.kotlin/kotlin-stdlib "kotlin-stdlib-${KV}.jar")" \
           "$(pick org.jetbrains.kotlin/kotlin-script-runtime "kotlin-script-runtime-${KV}.jar")" \
           "$(pick org.jetbrains.kotlin/kotlin-daemon-embeddable "kotlin-daemon-embeddable-${KV}.jar")" \
           "$(pick org.jetbrains.kotlin/kotlin-reflect 'kotlin-reflect-*.jar')" \
           "$(pick org.jetbrains.intellij.deps/trove4j 'trove4j-*.jar')" \
           "$(pick org.jetbrains.kotlinx/kotlinx-coroutines-core-jvm 'kotlinx-coroutines-core-jvm-*.jar')" \
           "$(pick org.jetbrains/annotations 'annotations-*.jar')"; do
    [ -n "${j}" ] && KOTLINC_CP="${KOTLINC_CP:+${KOTLINC_CP}:}${j}"
  done
  case "${KOTLINC_CP}" in *kotlin-compiler-embeddable*) ;; *) KOTLINC_CP="" ;; esac
fi
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
