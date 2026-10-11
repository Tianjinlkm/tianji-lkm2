#!/usr/bin/env bash
set -u
WS="${GITHUB_WORKSPACE:-$(pwd)}"; SRC="$WS/kernel-src"
BRANCH="${KERNEL_BRANCH:-pearl-s-oss}"
TGT="${TARGET_RELEASE:-5.10.236-android12-9-00003-gfb24cf99ad97-ab14313284}"
export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
export KBUILD_BUILD_TIMESTAMP="${KBUILD_BUILD_TIMESTAMP:-Tue Oct 21 03:03:12 UTC 2025}"
M="make -j$(nproc)"
L(){ echo; echo "===== $* ====="; }
E(){ echo "----- 报错 -----"; grep -inE 'error|no such file|cannot find' "$1" | head -20; tail -20 "$1"; }
L "1. 克隆 $BRANCH"
rm -rf "$SRC"; git clone --depth 1 https://github.com/MiCode/Xiaomi_Kernel_OpenSource.git -b "$BRANCH" "$SRC" || exit 1
cd "$SRC" || exit 1; grep -E '^(VERSION|PATCHLEVEL|SUBLEVEL|EXTRAVERSION) ' Makefile
L "2. 补全 Kconfig"
grep -rh 'source "' --include='Kconfig*' . 2>/dev/null | sed 's/.*source "//;s/".*//' | grep -v '^$' | grep -v '\$' | sort -u > /tmp/s.txt
echo "条目 $(wc -l < /tmp/s.txt)"
while read -r f; do if [ ! -e "$f" ]; then mkdir -p "$(dirname "$f")"; touch "$f"; echo "补全 $f"; fi; done < /tmp/s.txt
L "3. dtc 修复"
for f in scripts/dtc/dtc-lexer.l scripts/dtc/dtc-lexer.lex.c_shipped scripts/dtc/dtc-lexer.lex.c; do if [ -f "$f" ]; then sed -i '/^YYLTYPE yylloc;$/d' "$f"; echo "修补 $f"; fi; done
printf '\nHOSTCFLAGS += -fcommon\nKBUILD_HOSTCFLAGS += -fcommon\nHOST_EXTRACFLAGS += -fcommon\n' >> Makefile
L "4. .config"
CFG=""
for n in .config config config.gz .config.gz; do if [ -f "$WS/$n" ]; then CFG="$WS/$n"; break; fi; done
if [ -z "$CFG" ]; then echo "!! 无 .config"; exit 1; fi
echo "使用 $CFG"
if gzip -t "$CFG" 2>/dev/null; then gunzip -c "$CFG" > .config; else cp "$CFG" .config; fi
echo "行数 $(wc -l < .config)"
L "5. 版本对齐"
NUM=$(echo "$TGT" | sed -E 's/^([0-9]+\.[0-9]+\.[0-9]+).*/\1/'); SUF=$(echo "$TGT" | sed -E 's/^[0-9]+\.[0-9]+\.[0-9]+//')
sed -i -E "s/^SUBLEVEL *= *[0-9]+/SUBLEVEL = $(echo "$NUM" | cut -d. -f3)/" Makefile
sed -i -E 's/^EXTRAVERSION *=.*/EXTRAVERSION =/' Makefile
sed -i '/^CONFIG_LOCALVERSION=/d' .config; printf 'CONFIG_LOCALVERSION="%s"\n' "$SUF" >> .config
L "6. Clang->GCC 开关"
sed -i '/^CONFIG_CC_IS_CLANG=/d;/^CONFIG_CC_VERSION_TEXT=/d' .config
for s in GCC_PLUGINS LTO_CLANG_FULL LTO_CLANG_THIN THINLTO CFI_CLANG CFI_PERMISSIVE CFI_ICALL CFI_FUNCPTR SHADOW_CALL_STACK STACKPROTECTOR_PER_TASK TRIM_UNUSED_KSYMS MODULE_SIG_ALL WERROR DEBUG_INFO_BTF; do scripts/config --disable "$s" 2>/dev/null || true; done
for s in LTO_NONE MODULES SMP PREEMPT; do scripts/config --enable "$s" 2>/dev/null || true; done
$M olddefconfig || { echo "!! olddefconfig 失败"; exit 1; }
if ! grep -q '^CONFIG_MODULES=y' .config; then echo "!! CONFIG_MODULES 未开"; exit 1; fi
GOT="$(sed -n 's/^VERSION *= *//p' Makefile | head -1).$(sed -n 's/^PATCHLEVEL *= *//p' Makefile | head -1).$(sed -n 's/^SUBLEVEL *= *//p' Makefile | head -1)$(sed -n 's/^EXTRAVERSION *= *//p' Makefile | head -1)$(sed -n 's/^CONFIG_LOCALVERSION="\(.*\)"/\1/p' .config | head -1)"
echo "KERNELRELEASE = $GOT"; echo "目标          = $TGT"
if [ "$GOT" = "$TGT" ]; then echo "==> 版本一致"; else echo "==> 版本不一致!"; fi
L "7. modules_prepare (四级降级)"
k1(){ echo ">>> 禁用 dtc"; if [ -f scripts/dtc/Makefile ]; then sed -i -E 's@^([[:space:]]*)(hostprogs-[A-Za-z0-9_$()$-]*|always|subdir-[A-Za-z0-9_$()$-]*)[[:space:]]*(:=|\+=)@\1# OFF \2 \3@' scripts/dtc/Makefile; fi; find scripts/dtc -name '*.o' -delete 2>/dev/null; }
k2(){ echo ">>> 跳过 usr/include"; printf '\nheaders_install:\n\t@true\nheaders_install_all:\n\t@true\nheaders:\n\t@true\n' >> Makefile; }
k3(){ echo ">>> 手工分步"; for t in include/config/kernel.release include/generated/utsrelease.h include/generated/autoconf.h include/generated/compile.h include/generated/bounds.h archprepare arch/arm64/include/generated/asm-offsets.h scripts/module.lds prepare modules_prepare; do $M "$t" || true; done; }
if $M modules_prepare > /tmp/p1.log 2>&1; then echo "==> 第1次通过"
elif k1 && $M modules_prepare > /tmp/p2.log 2>&1; then echo "==> 第2次通过(禁用 dtc)"
elif k2 && $M modules_prepare > /tmp/p3.log 2>&1; then echo "==> 第3次通过(跳过 headers)"
else echo "!!! 前3次都失败"; echo "## p1 ##"; E /tmp/p1.log; echo "## p2 ##"; E /tmp/p2.log; echo "## p3 ##"; E /tmp/p3.log; k3; fi
L "8. 自检"
for f in include/config/auto.conf include/config/kernel.release include/generated/autoconf.h include/generated/utsrelease.h arch/arm64/include/generated/asm/errno.h arch/arm64/include/generated/asm-offsets.h; do if [ -e "$f" ]; then echo "OK      $f"; else echo "MISSING $f"; fi; done
echo "--- kernel.release ---"; cat include/config/kernel.release 2>/dev/null
L "9. 编译模块"
ls -1 "$WS"/*.c 2>/dev/null; echo "--- Makefile ---"; cat "$WS/Makefile" 2>/dev/null || echo "(无)"
if [ ! -f "$WS/Makefile" ]; then C=$(ls -1 "$WS"/*.c 2>/dev/null | head -1); if [ -n "${C:-}" ]; then printf 'obj-m += %s.o\nccflags-y += -Wall -Wno-unused-function -fno-stack-protector\n' "$(basename "$C" .c)" > "$WS/Makefile"; echo ">>> 自动生成"; cat "$WS/Makefile"; else echo "!! 无 Makefile 无 .c"; fi; fi
make -j"$(nproc)" KBUILD_MODPOST_WARN=1 -C "$SRC" M="$WS" modules 2>&1 | tee /tmp/m.log | tail -50
RC=${PIPESTATUS[0]}
if [ "$RC" -eq 0 ]; then echo "==> 编译成功"; else echo "!!! 编译失败 (码 $RC)"; E /tmp/m.log; fi
L "10. 收集 .ko"
mkdir -p "$WS/out"; find "$WS" -maxdepth 1 -name '*.ko' -type f > /tmp/k.txt
if [ ! -s /tmp/k.txt ]; then find "$WS" -name '*.ko' -type f 2>/dev/null | grep -v '/kernel-src/' > /tmp/k.txt; fi
while read -r ko; do cp -f "$ko" "$WS/out/" && echo "收集 $ko"; done < /tmp/k.txt
ls -la "$WS/out/"
L "11. vermagic"
echo "目标: $TGT SMP PREEMPT $KBUILD_BUILD_TIMESTAMP"
R=$(command -v readelf || command -v aarch64-linux-gnu-readelf || true)
for ko in "$WS"/out/*.ko; do if [ ! -e "$ko" ]; then echo "!! 没有 .ko"; break; fi; echo "== $(basename "$ko") =="; if [ -n "${R:-}" ]; then "$R" -p .modinfo "$ko" | head -20; if "$R" -p .modinfo "$ko" | grep -q "$TGT"; then echo "OK 版本一致"; else echo "XX 版本不一致"; fi; fi; done
echo; echo "===== 完毕 ====="
# BUILD-SH-END-MARKER
