#!/usr/bin/env bash
# Hermetic test PKI: a private CA plus leaf certificates for the TLS fixture
# server (good, expired, wrong host, self-signed, signed by an untrusted CA).
# Usage: gen_certs.sh <output dir>
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
d=$1; mkdir -p "$d"; cd "$d"
rm -f ./*.pem ./*.key ./*.csr ./*.srl index.txt* serial.txt* ./*.cnf ./*.ext
CAEXT="basicConstraints=critical,CA:TRUE
keyUsage=critical,keyCertSign,cRLSign"
mkca() { # name
  openssl req -x509 -newkey rsa:2048 -nodes -keyout "$1.key" -out "$1.pem" -days 30 \
    -subj "/CN=Wings Test $1" -addext "basicConstraints=critical,CA:TRUE" \
    -addext "keyUsage=critical,keyCertSign,cRLSign" 2>/dev/null
}
printf 'subjectAltName=DNS:art.test,DNS:cdn.test,IP:127.0.0.1\nextendedKeyUsage=serverAuth\n' > good.ext
printf 'subjectAltName=DNS:other.test\nextendedKeyUsage=serverAuth\n' > wrong.ext
mkca ca
mkca otherca
leaf() { # name signer ext
  openssl req -newkey rsa:2048 -nodes -keyout "$1.key" -out "$1.csr" -subj "/CN=art.test" 2>/dev/null
  openssl x509 -req -in "$1.csr" -CA "$2.pem" -CAkey "$2.key" -CAcreateserial -out "$1.pem" \
    -days 30 -extfile "$3" 2>/dev/null
}
leaf good ca good.ext
leaf wronghost ca wrong.ext
leaf untrusted otherca good.ext
# self-signed leaf
openssl req -x509 -newkey rsa:2048 -nodes -keyout selfsigned.key -out selfsigned.pem -days 30 \
  -subj "/CN=art.test" -addext "subjectAltName=DNS:art.test,IP:127.0.0.1" 2>/dev/null
# expired leaf (needs `openssl ca` for explicit dates)
: > index.txt; echo 01 > serial.txt
cat > ca.cnf <<CNF
[ca]
default_ca=CA_default
[CA_default]
dir=.
database=index.txt
new_certs_dir=.
serial=serial.txt
default_md=sha256
policy=pol
unique_subject=no
[pol]
commonName=supplied
CNF
openssl req -newkey rsa:2048 -nodes -keyout expired.key -out expired.csr -subj "/CN=art.test" 2>/dev/null
openssl ca -batch -notext -config ca.cnf -cert ca.pem -keyfile ca.key -in expired.csr -out expired.pem \
  -startdate 20200101000000Z -enddate 20200201000000Z -extfile good.ext 2>/dev/null
echo "test PKI in $d"
