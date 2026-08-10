#!/usr/bin/env bash
# Generate the TLS material the firmware embeds.
#
#   ./tools/make_certs.sh                      # create a local CA if needed, then a device cert
#   ./tools/make_certs.sh --host kvm-rack      # a second dongle, different mDNS name
#   ./tools/make_certs.sh --ca-cert ca.crt --ca-key ca.key   # sign with a CA you already run
#
# Why a CA at all, rather than a self-signed device certificate: the browser has
# to consider the origin trustworthy, not merely encrypted. Chrome now resists
# accepting self-signed certificates by default and restricts some powerful APIs
# even after a click-through, so a self-signed cert risks paying the whole cost
# of TLS and still leaving getUserMedia (the capture-device dropdown) empty.
# Trusting one CA once per host is a smaller, more honest ask than teaching
# yourself to click through certificate warnings.
#
# Output lands in certs/, which is gitignored. The CA private key generated here
# can mint a certificate for ANY name your machines trust — treat it like an ssh
# private key, and prefer passing an existing CA you already manage.
set -euo pipefail

cd "$(dirname "$0")/.."
CERT_DIR="certs"
HOST="airkvm"
CA_CERT=""
CA_KEY=""
DAYS=825   # the longest most browsers accept for a leaf certificate

while [[ $# -gt 0 ]]; do
  case "$1" in
    --host)    HOST="$2"; shift 2 ;;
    --ca-cert) CA_CERT="$2"; shift 2 ;;
    --ca-key)  CA_KEY="$2"; shift 2 ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

mkdir -p "$CERT_DIR"

if [[ -z "$CA_CERT" ]]; then
  CA_CERT="$CERT_DIR/ca.crt"
  CA_KEY="$CERT_DIR/ca.key"
  if [[ ! -f "$CA_CERT" ]]; then
    echo "==> creating a local CA (no --ca-cert given)"
    # 10 years: this CA gets installed by hand on each machine you browse from,
    # and re-doing that annually is how people end up disabling checks instead.
    openssl req -x509 -newkey rsa:2048 -nodes -keyout "$CA_KEY" -out "$CA_CERT" \
      -days 3650 -subj "/CN=airkvm local CA" \
      -addext "basicConstraints=critical,CA:TRUE" \
      -addext "keyUsage=critical,keyCertSign,cRLSign" 2>/dev/null
    chmod 600 "$CA_KEY"
    echo "    created $CA_CERT"
  else
    echo "==> reusing existing CA at $CA_CERT"
  fi
fi

if [[ ! -f "$CA_KEY" ]]; then
  echo "CA key not found: $CA_KEY" >&2
  exit 1
fi

KEY="$CERT_DIR/${HOST}.key"
CRT="$CERT_DIR/${HOST}.crt"

echo "==> issuing a certificate for ${HOST}.local"
# RSA rather than an EC key: the ESP32-C3 has an MPI (big-integer) accelerator
# that RSA uses and no ECC accelerator, so RSA handshakes are the faster of the
# two on this chip despite being the heavier algorithm on paper.
openssl req -newkey rsa:2048 -nodes -keyout "$KEY" -out "$CERT_DIR/${HOST}.csr" \
  -subj "/CN=${HOST}.local" 2>/dev/null

# The SAN is what browsers actually check; a CN alone has not been accepted for
# years. Both the mDNS name and the bare hostname are listed so either URL works.
openssl x509 -req -in "$CERT_DIR/${HOST}.csr" -CA "$CA_CERT" -CAkey "$CA_KEY" \
  -CAcreateserial -out "$CRT" -days "$DAYS" -sha256 \
  -extfile <(printf 'subjectAltName=DNS:%s.local,DNS:%s\nextendedKeyUsage=serverAuth\n' "$HOST" "$HOST") \
  2>/dev/null
rm -f "$CERT_DIR/${HOST}.csr"
chmod 600 "$KEY"

# The firmware embeds fixed filenames, so the active pair is copied into place.
# This keeps multi-device certificates side by side while the build stays simple.
cp "$CRT" "$CERT_DIR/device.crt"
cp "$KEY" "$CERT_DIR/device.key"

echo
echo "certificate ready for https://${HOST}.local/"
echo "  device cert : $CRT  (copied to certs/device.crt for the build)"
echo "  CA to trust : $CA_CERT"
echo
echo "Install the CA once per machine you browse from:"
echo "  macOS: sudo security add-trusted-cert -d -r trustRoot \\"
echo "           -k /Library/Keychains/System.keychain $CA_CERT"
echo "  Linux: sudo cp $CA_CERT /usr/local/share/ca-certificates/airkvm-ca.crt \\"
echo "           && sudo update-ca-certificates"
echo
echo "Then: mise run build && mise run flash"
