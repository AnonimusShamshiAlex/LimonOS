#!/bin/bash
# Сквозная проверка сетевого стека LimonOS на хосте: TAP-интерфейс + Python-сервер (http и https).
# Нужен root (создаёт TAP). Проверяет загрузку, https, redirect, chunked, POST, потерю пакетов.
cd "$(dirname "$0")/.."
T=/tmp/nstest; rm -rf $T; mkdir -p $T; cd $T
head -c 3000000 /dev/urandom > big.bin; head -c 1000000 /dev/urandom > chunk.bin; head -c 700000 /dev/urandom > up.bin
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout ec.key -out ec.crt -subj "/CN=10.0.2.1" -days 2 2>/dev/null
cd - >/dev/null
python3 - <<'PY'
import os, fcntl, struct, socket
fd = os.open("/dev/net/tun", os.O_RDWR)
fcntl.ioctl(fd, 0x400454ca, struct.pack("16sH", b"limon0", 0x0002 | 0x1000))
fcntl.ioctl(fd, 0x400454cb, 1)   # persist
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
fcntl.ioctl(s, 0x8916, struct.pack("16sH2s4s8x", b"limon0", socket.AF_INET, b"\0\0", socket.inet_aton("10.0.2.1")))
fcntl.ioctl(s, 0x891c, struct.pack("16sH2s4s8x", b"limon0", socket.AF_INET, b"\0\0", socket.inet_aton("255.255.255.0")))
fl = struct.unpack("16sH14x", fcntl.ioctl(s, 0x8913, struct.pack("16sH14x", b"limon0", 0)))[1]
fcntl.ioctl(s, 0x8914, struct.pack("16sH14x", b"limon0", fl | 1))
PY
gcc -O2 -Wall -Wextra -Wno-unused-parameter -I kernel -I tests tests/netstack_host.c tests/nic_tap.c kernel/net.c kernel/tls.c kernel/crypto.c kernel/http.c kernel/format.c -o $T/ns || exit 1
python3 tests/test_server.py $T > $T/server.log 2>&1 &
SP=$!; sleep 1.5
sha() { sha256sum "$1" | cut -d' ' -f1; }
pass=0; fail=0
run() {  # имя, ожидаемый файл (или -), url, [env], [POST файл]
  local name="$1" want="$2" url="$3" loss="$4"; shift 4
  rm -f $T/out
  local t0=$(date +%s.%N)
  TMO=$([ "$loss" -ge 5 ] && echo 40000 || echo 8000) LOSS=$loss timeout 170 $T/ns "$url" $T/out "$@" 2> $T/err.txt; local rc=$?
  local dt=$(printf "%.1f" $(echo "$(date +%s.%N) - $t0" | bc))
  local ok=0
  if [ $rc = 0 ]; then
    if [ "$want" = "-" ]; then ok=1; else [ "$(sha $T/out)" = "$(sha $want)" ] && ok=1; fi
  fi
  if [ $ok = 1 ]; then echo "  ok   $name (${dt} с) $(tail -1 $T/err.txt | cut -c1-150)"; pass=$((pass+1)); else echo "  FAIL $name rc=$rc: $(tail -2 $T/err.txt)"; fail=$((fail+1)); fi
}
run "http  3 МБ без потерь"            $T/big.bin   http://10.0.2.1:8080/big.bin 0
run "https 3 МБ без потерь"            $T/big.bin   https://10.0.2.1:8443/big.bin 0
run "http  3 МБ, потеря 3% пакетов"    $T/big.bin   http://10.0.2.1:8080/big.bin 3
run "https 3 МБ, потеря 3% пакетов"    $T/big.bin   https://10.0.2.1:8443/big.bin 3
run "http  chunked 1 МБ (куски 1..70000 байт)" $T/chunk.bin http://10.0.2.1:8080/chunked 0
run "https chunked 1 МБ, потеря 2%"    $T/chunk.bin https://10.0.2.1:8443/chunked 2
run "http  без Content-Length (конец по закрытию)" - http://10.0.2.1:8080/eof 0
run "http  redirect 302 -> /big.bin"   $T/big.bin   http://10.0.2.1:8080/redir 0
run "https redirect 301 (относительный)" $T/big.bin https://10.0.2.1:8443/redir2 0
rm -f $T/out; LOSS=0 $T/ns https://10.0.2.1:8443/downgrade $T/out 2> $T/err.txt
if grep -q "ОШИБКА -11" $T/err.txt; then echo "  ok   https -> http redirect отклонён (защита от понижения шифрования)"; pass=$((pass+1)); else echo "  FAIL downgrade не отклонён"; fail=$((fail+1)); fi
run "http  404"                        -            http://10.0.2.1:8080/nope 0
run "https POST 700 КБ (upload)"       -            https://10.0.2.1:8443/echo 0 POST $T/up.bin
[ "$(cat $T/out)" = "$(sha $T/up.bin)" ] && echo "       хеш загруженного совпал с хешем на сервере" || { echo "  FAIL: POST хеш не совпал"; fail=$((fail+1)); }
run "http  3 МБ, потеря 5% (жёсткий стресс)"  $T/big.bin   http://10.0.2.1:8080/big.bin 5
run "https 3 МБ, потеря 5% (жёсткий стресс)"  $T/big.bin   https://10.0.2.1:8443/big.bin 5
run "http  POST 700 КБ, потеря 3%"     -            http://10.0.2.1:8080/echo 3 POST $T/up.bin
[ "$(cat $T/out)" = "$(sha $T/up.bin)" ] && echo "       хеш загруженного совпал" || { echo "  FAIL: POST хеш не совпал (потери)"; fail=$((fail+1)); }
kill $SP 2>/dev/null
echo "ПРОШЛО: $pass, ПРОВАЛОВ: $fail"; [ $fail -le 0 ]
