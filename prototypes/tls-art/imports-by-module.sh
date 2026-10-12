cd /root/build/probe/app
L=.deps/native/ps5-payload-sdk/target/lib
# undefined symbols of the linked ELF (imports)
readelf -W --dyn-syms build/llvm-pie.elf | awk '$7=="UND" && $4=="FUNC"{print $8}' | sed 's/@.*//' | sort -u > /tmp/und.txt
wc -l < /tmp/und.txt
# link order as in the command line (shell glob order)
for f in .deps/ps5-opengl/current/lib/libSceAgc.so .deps/ps5-opengl/current/lib/libSceAgcDriver.so $L/libSce*.so $L/libc_stub_weak.so $L/libkernel.so $L/libkernel_stub_weak.so $L/libkernel_sys.so $L/libkernel_web.so $L/libmonosgen-2.0.so; do
  nm -D --defined-only "$f" 2>/dev/null | awk '{print $3}' | sed 's/@.*//' | sort -u > /tmp/def.txt
  comm -12 /tmp/und.txt /tmp/def.txt > /tmp/hit.txt
  n=$(wc -l < /tmp/hit.txt)
  if [ "$n" -gt 0 ]; then echo "== $(basename $f): $n"; if [ "$(basename $f)" = libScePosixForWebKit.so ] || [ "$(basename $f)" = libSceNet.so ]; then cat /tmp/hit.txt | tr '\n' ' '; echo; fi; comm -23 /tmp/und.txt /tmp/hit.txt > /tmp/und2.txt; fi
  # symbols already claimed by an earlier stub are removed from consideration (first definition wins)
  [ -s /tmp/hit.txt ] && { comm -23 /tmp/und.txt /tmp/hit.txt > /tmp/und2.txt; cp /tmp/und2.txt /tmp/und.txt; }
done
echo "unresolved leftovers (other objects/weak):"; wc -l < /tmp/und.txt; head -20 /tmp/und.txt | tr '\n' ' '
