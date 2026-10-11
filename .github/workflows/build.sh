#!/usr/bin/env bash
# ARM64 LKM 云编译脚本（Redmi Note 12T Pro / pearl）
# 所有编译逻辑都在这个文件里，workflow 只负责调用它。
set -u

WS="${GITHUB_WORKSPACE:-$(pwd)}"
SRC="$WS/kernel-src"
BRANCH="${KERNEL_BRANCH:-pearl-s-oss}"
TGT="${TARGET_RELEASE:-5.10.236-android12-9-00003-gfb24cf99ad97-ab14313284}"

export ARCH=arm64
export CROSS_COMPILE=aarch64-linux-gnu-
export KBUILD_BUILD_TIMESTAMP="${KBUILD_BUILD_TIMESTAMP:-Tue Oct 21 03:03:12 UTC 2025}"
MAKE="make -j$(nproc)"

log() { echo; echo "==================== $* ===================="; }

log "1. 克隆内核源码 ($BRANCH)"
rm -rf "$SRC"
git clone --depth 1 https://github.com/MiCode/Xiaomi_Kernel_OpenSource.git -b "$BRANCH" "$SRC" || exit 1
cd "$SRC" || exit 1
grep -E '^(VERSION|PATCHLEVEL|SUBLEVEL|EXTRAVERSION) ' Makefile

log "2. 补全缺失的 Kconfig"
grep -rh 'source "' --include='Kconfig*' . 2>/dev/null \
  | sed 's/.*source "//; s/".*//' | grep -v '^$' | grep -v '\$' | sort -u > /tmp/srcs.txt
echo "扫描到 source 条目: $(wc -l < /tmp/srcs.txt)"
while read -r f; do
  if [ ! -e "$f" ]; then
    mkdir -p "$(dirname "$f")"
    touch "$f"
    echo "补全: $f"
  fi
done < /tmp/srcs.txt

log "3. dtc 修复 (GCC10+ -fno-common 导致 yylloc 重复定义)"
for f in scripts/dtc/dtc-lexer.l scripts/dtc/dtc-lexer.lex.c_shipped scripts/dtc/dtc-lexer.lex.c; do
  if [ -f "$f" ]; then
    sed -i '/^YYLTYPE yylloc;$/d' "$f"
    echo "修补: $f"
  fi
done
printf '\nHOSTCFLAGS += -fcommon\nKBUILD_HOSTCFLAGS += -fcommon\nHOST_EXTRACFLAGS += -fcommon\n' >> Makefile

log "4. 准备 .config"
CFG=""
for n in .config config config.gz .config.gz; do
  if [ -f "$WS/$n" ]; then CFG="$WS/$n"; break; fi
done
if [ -z "$CFG" ]; then echo "!! 仓库根目录没有 .config"; exit 1; fi
echo "使用 config: $CFG"
if gzip -t "$CFG" 2>/dev/null; then
  gunzip -c "$CFG" > .config
else
  cp "$CFG" .config
fi
echo ".config 行数: $(wc -l < .config)"

log "5. 版本对齐 (vermagic 必须和手机一模一样)"
NUM=$(echo "$TGT" | sed -E 's/^([0-9]+\.[0-9]+\.[0-9]+).*/\1/')
SUF=$(echo "$TGT" | sed -E 's/^[0-9]+\.[0-9]+\.[0-9]+//')
sed -i -E "s/^SUBLEVEL *= *[0-9]+/SUBLEVEL = $(echo "$NUM" | cut -d. -f3)/" Makefile
sed -i -E 's/^EXTRAVERSION *=.*/EXTRAVERSION =/' Makefile
sed -i '/^CONFIG_LOCALVERSION=/d' .config
printf 'CONFIG_LOCALVERSION="%s"\n' "$SUF" >> .config

log "6. 调整 config 开关 (设备 config 是 Clang 的，换 GCC 要摘掉)"
sed -i '/^CONFIG_CC_IS_CLANG=/d; /^CONFIG_CC_VERSION_TEXT=/d' .config
for s in GCC_PLUGINS LTO_CLANG_FULL LTO_CLANG_THIN THINLTO \
         CFI_CLANG CFI_PERMISSIVE CFI_ICALL CFI_FUNCPTR \
         SHADOW_CALL_STACK STACKPROTECTOR_PER_TASK TRIM_UNUSED_KSYMS \
         MODULE_SIG_ALL WERROR DEBUG_INFO_BTF; do
  scripts/config --disable "$s" 2>/dev/null || true
done
for s in LTO_NONE MODULES SMP PREEMPT; do
  scripts/config --enable "$s" 2>/dev/null || true
done
$MAKE olddefconfig || { echo "!! olddefconfig 失败"; exit 1; }
if ! grep -q '^CONFIG_MODULES=y' .config; then echo "!! CONFIG_MODULES 未开"; exit 1; fi
V=$(sed -n 's/^VERSION *= *//p' Makefile | head -1)
P=$(sed -n 's/^PATCHLEVEL *= *//p' Makefile | head -1)
S=$(sed -n 's/^SUBLEVEL *= *//p' Makefile | head -1)
E=$(sed -n 's/^EXTRAVERSION *= *//p' Makefile | head -1)
L=$(sed -n 's/^CONFIG_LOCALVERSION="\(.*\)"/\1/p' .config | head -1)
GOT="${V}.${P}.${S}${E}${L}"
echo "KERNELRELEASE = $GOT"
echo "目标          = $TGT"
if [ "$GOT" = "$TGT" ]; then echo "==> 版本一致"; else echo "==> 版本不一致!"; fi

log "7. modules_prepare"
if ! $MAKE modules_prepare > /tmp/prep1.log 2>&1; then
  echo "--- 失败，真实错误 ---"
  tail -60 /tmp/prep1.log
  echo "--- 禁用 dtc 后重试 ---"
  if [ -f scripts/dtc/Makefile ]; then
    sed -i -E 's@^([[:space:]]*)(hostprogs-[A-Za-z0-9_$()$-]*|always|subdir-[A-Za-z0-9_$()$-]*)[[:space:]]*(:=|\+=)@\1# DISABLED-DTC \2 \3@' scripts/dtc/Makefile
    cat scripts/dtc/Makefile
  fi
  find scripts/dtc -name '*.o' -delete 2>/dev/null
  if ! $MAKE modules_prepare > /tmp/prep2.log 2>&1; then
    echo "!! modules_prepare 彻底失败"
    tail -80 /tmp/prep2.log
    exit 1
  fi
fi

log "8. 产物自检"
for f in include/config/auto.conf include/config/kernel.release \
         include/generated/autoconf.h include/generated/utsrelease.h \
         arch/arm64/include/generated/asm/errno.h \
         arch/arm64/include/generated/asm-offsets.h; do
  if [ -e "$f" ]; then echo "OK      : $f"; else echo "MISSING : $f"; fi
done
echo "--- kernel.release ---"
cat include/config/kernel.release 2>/dev/null

log "9. 编译模块"
echo "--- 模块目录 ---"
ls -la "$WS"
echo "--- 模块 Makefile ---"
cat "$WS/Makefile" 2>/dev/null || echo "(根目录没有 Makefile)"
make -j"$(nproc)" KBUILD_MODPOST_WARN=1 -C "$SRC" M="$WS" modules 2>&1 | tail -60

log "10. 收集 .ko"
mkdir -p "$WS/out"
find "$WS" -maxdepth 2 -name '*.ko' -type f > /tmp/kos.txt
if [ ! -s /tmp/kos.txt ]; then
  find "$WS" -name '*.ko' -type f 2>/dev/null | grep -v '/kernel-src/' > /tmp/kos.txt
fi
while read -r ko; do
  cp -f "$ko" "$WS/out/" && echo "收集: $ko"
done < /tmp/kos.txt
echo "--- out/ ---"
ls -la "$WS/out/"

log "11. vermagic 核对"
for ko in "$WS"/out/*.ko; do
  if [ ! -e "$ko" ]; then echo "!! 没有生成任何 .ko"; break; fi
  echo "== $ko =="
  readelf -p .modinfo "$ko" | head -20
done

echo
echo "==================== 脚本执行完毕 ===================="
exit 0
