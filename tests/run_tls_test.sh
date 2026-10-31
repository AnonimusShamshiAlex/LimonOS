#!/bin/bash
# Настоящий OpenSSL-сервер на localhost + наш TLS-клиент. Проверяем TLS 1.3 и TLS 1.2, оба набора шифров, два типа сертификатов и возобновление сессии.
set -e
cd "$(dirname "$0")/.."
T=/tmp/tlstest; rm -rf $T; mkdir -p $T/www; cd $T
head -c 3000000 /dev/urandom > www/big.bin
echo "hello from openssl" > www/hello.txt
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout ec.key -out ec.crt -subj "/CN=localhost" -days 2 2>/dev/null
openssl req -x509 -newkey rsa:2048 -nodes -keyout rsa.key -out rsa.crt -subj "/CN=localhost" -days 2 2>/dev/null
cd - >/dev/null
gcc -O2 -Wall -Wextra -I kernel -I tests tests/tls_host_test.c kernel/tls.c kernel/crypto.c kernel/string.c -o $T/client
pass=0; fail=0
run_case() {  # имя, аргументы сервера, аргументы клиента..., (сертификат берём из $cert)
  local name="$1" sargs="$2"; shift 2
  port=$((4400 + RANDOM % 500))
  (cd $T/www && exec openssl s_server -accept $port -cert ../$cert.crt -key ../$cert.key $sargs -WWW -quiet >/dev/null 2>&1) &
  SP=$!; sleep 0.7
  if $T/client $port /big.bin "$@" > $T/out.raw 2> $T/err.txt; then
    python3 - "$T" <<'PY' && ok=1 || ok=0
import sys,hashlib
T=sys.argv[1]; d=open(T+"/out.raw","rb").read(); i=d.index(b"\r\n\r\n")+4
sys.exit(0 if hashlib.sha256(d[i:]).digest()==hashlib.sha256(open(T+"/www/big.bin","rb").read()).digest() else 1)
PY
  else ok=0; fi
  info=$(grep рукопожатие $T/err.txt | tail -1 || true)
  if [ "$ok" = 1 ]; then echo "  ok   $name  ($info), 3 МБ совпали побайтно"; pass=$((pass+1)); else echo "  FAIL $name"; cat $T/err.txt; fail=$((fail+1)); fi
  kill $SP 2>/dev/null; wait $SP 2>/dev/null || true
}
for cert in ec rsa; do for suite in TLS_CHACHA20_POLY1305_SHA256 TLS_AES_128_GCM_SHA256; do
  run_case "TLS1.3 $cert / $suite" "-tls1_3 -ciphersuites $suite"
done; done
for cert in ec rsa; do
  if [ $cert = ec ]; then c1=ECDHE-ECDSA-AES128-GCM-SHA256; c2=ECDHE-ECDSA-CHACHA20-POLY1305; else c1=ECDHE-RSA-AES128-GCM-SHA256; c2=ECDHE-RSA-CHACHA20-POLY1305; fi
  for c in $c1 $c2; do
    run_case "TLS1.2 $cert / $c (клиент предлагает 1.3+1.2)" "-tls1_2 -cipher $c -groups X25519"
    run_case "TLS1.2 $cert / $c сервер выбирает P-256" "-tls1_2 -cipher $c -groups P-256" only12
    run_case "TLS1.2 $cert / $c (только 1.2)" "-tls1_2 -cipher $c -groups X25519" only12
    run_case "TLS1.2 $cert / $c возобновление сессии" "-tls1_2 -cipher $c -groups X25519" only12 resume
  done
done
echo "ПРОШЛО: $pass, ПРОВАЛОВ: $fail"; [ $fail = 0 ]
