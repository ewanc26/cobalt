#!/bin/sh
#
# Fetch the CA trust store Cobalt ships in its romfs.
#
# Why this exists
# ---------------
# devkitPro's wiiu-curl is built against mbedTLS, and the Wii U has no system
# certificate store for it to fall back on. Without an explicit CURLOPT_CAINFO
# every HTTPS request fails verification, which means no XRPC at all — this is
# the first thing that blocks AGENTS.md §12's step 2. Wolfram exposes the knob
# as wf_xrpc_client_set_ca_bundle(); this script provides the file it points at.
#
# The bundle is deliberately NOT committed. It expires, it is large, and a stale
# copy fails in a way that looks like a network bug rather than an out-of-date
# trust store. Fetch it at build time instead (the Makefile does this for you)
# and re-run this script when certificates start being rejected.
#
# Usage:
#   tools/fetch_cacert.sh [output-path]
#
# Environment:
#   COBALT_CACERT_SOURCE=system   Copy the build machine's own trust store
#                                 instead of downloading. Useful offline, but
#                                 note it inherits whatever that machine trusts,
#                                 including any corporate/proxy MITM roots — do
#                                 not ship a bundle built this way.
#
set -eu

OUT="${1:-romfs/cacert.pem}"
URL="https://curl.se/ca/cacert.pem"

# The Mozilla set is a little over 100 certificates; anything far below that is
# a truncated download or an error page that happened to save successfully.
MIN_CERTS=50

TMP="${OUT}.tmp.$$"
cleanup() { rm -f "$TMP"; }
trap cleanup EXIT INT TERM

mkdir -p "$(dirname "$OUT")"

fetch_system() {
   for candidate in \
      /etc/ssl/certs/ca-certificates.crt \
      /etc/pki/tls/certs/ca-bundle.crt \
      /etc/ssl/cert.pem \
      /usr/local/etc/openssl/cert.pem
   do
      if [ -r "$candidate" ]; then
         echo "fetch_cacert: copying $candidate" >&2
         cat "$candidate" > "$TMP"
         return 0
      fi
   done
   echo "fetch_cacert: no system trust store found" >&2
   return 1
}

fetch_remote() {
   if command -v curl >/dev/null 2>&1; then
      echo "fetch_cacert: downloading $URL" >&2
      curl -fsS --proto '=https' --tlsv1.2 -o "$TMP" "$URL"
   elif command -v wget >/dev/null 2>&1; then
      echo "fetch_cacert: downloading $URL" >&2
      wget -q --https-only -O "$TMP" "$URL"
   else
      echo "fetch_cacert: neither curl nor wget is available" >&2
      return 1
   fi
}

if [ "${COBALT_CACERT_SOURCE:-remote}" = "system" ]; then
   fetch_system
else
   if ! fetch_remote; then
      echo "fetch_cacert: download failed." >&2
      echo "  Retry with network access, or re-run with" >&2
      echo "  COBALT_CACERT_SOURCE=system to use this machine's own store." >&2
      exit 1
   fi
fi

# Validate before installing. A half-written or HTML-error-page bundle would
# otherwise be discovered on the console, which is the expensive place to find
# out (AGENTS.md §10 — there is no emulator in this loop).
count=$(grep -c -- '-----BEGIN CERTIFICATE-----' "$TMP" || true)
if [ "${count:-0}" -lt "$MIN_CERTS" ]; then
   echo "fetch_cacert: refusing to install a bundle with only ${count:-0} certificates" >&2
   exit 1
fi

mv "$TMP" "$OUT"
trap - EXIT INT TERM
echo "fetch_cacert: wrote $OUT ($count certificates)" >&2
