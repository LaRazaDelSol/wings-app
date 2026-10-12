cd /root/build/probe/app
readelf -W --dyn-syms build/llvm-pie.elf | awk '$7=="UND"{print $8}' | sed 's/@.*//' | sort -u > /tmp/und_all.txt
echo "new vs stock:"
readelf -W --dyn-syms /root/build/orbit/app/build/llvm-pie.elf | awk '$7=="UND"{print $8}' | sed 's/@.*//' | sort -u > /tmp/und_stock.txt
comm -23 /tmp/und_all.txt /tmp/und_stock.txt | tr '\n' ' '
echo; echo "random/time related:"; grep -iE "random|entropy|arc4|getrandom|clock|time|urandom" /tmp/und_all.txt | tr '\n' ' '
