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

# Trim to the roots the AT Protocol network actually chains to.
#
# libcurl's mbedTLS backend re-parses the whole CA file on every new
# connection, and a Wii U's PowerPC cores take on the order of a second to chew
# through the ~120-certificate Mozilla set. That cost was paid on every XRPC
# call and every avatar fetch, so the app felt frozen. The roots below cover
# Let's Encrypt, DigiCert, Amazon, Google, GlobalSign, Sectigo, Cloudflare's and
# the other common issuers; COBALT_CACERT_FULL=1 keeps the complete set for a
# PDS on something exotic.
if [ "${COBALT_CACERT_FULL:-0}" != "1" ]; then
   FILTERED="${TMP}.f"
   awk '
      BEGIN {
         n = split("ISRG Root X1|ISRG Root X2|DigiCert Global Root CA|DigiCert Global Root G2|DigiCert Global Root G3|DigiCert TLS RSA4096 Root G5|DigiCert TLS ECC P384 Root G5|DigiCert High Assurance EV Root CA|Amazon Root CA 1|Amazon Root CA 2|Amazon Root CA 3|Amazon Root CA 4|GTS Root R1|GTS Root R2|GTS Root R3|GTS Root R4|GlobalSign Root CA|GlobalSign Root CA - R3|GlobalSign Root CA - R6|GlobalSign ECC Root CA - R4|GlobalSign ECC Root CA - R5|USERTrust RSA Certification Authority|USERTrust ECC Certification Authority|Starfield Services Root Certificate Authority - G2|Starfield Root Certificate Authority - G2|Go Daddy Root Certificate Authority - G2|Baltimore CyberTrust Root|COMODO RSA Certification Authority|COMODO ECC Certification Authority|SSL.com Root Certification Authority RSA|SSL.com TLS RSA Root CA 2022|SSL.com TLS ECC Root CA 2022|SSL.com Root Certification Authority ECC|Microsoft RSA Root Certificate Authority 2017|Microsoft ECC Root Certificate Authority 2017|Entrust Root Certification Authority - G2|Certum Trusted Network CA|Sectigo Public Server Authentication Root R46|Sectigo Public Server Authentication Root E46", names, "|")
         for (i = 1; i <= n; i++) want[names[i]] = 1
      }
      { lines[++c] = $0 }
      END {
         for (i = 1; i <= c; i++) {
            if (lines[i] ~ /^=+$/ && i > 1) label = lines[i-1]
            if (lines[i] == "-----BEGIN CERTIFICATE-----") { keep = (label in want) }
            if (keep) print lines[i]
            if (lines[i] == "-----END CERTIFICATE-----") keep = 0
         }
      }' "$TMP" > "$FILTERED"
   mv "$FILTERED" "$TMP"
   MIN_CERTS=20
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
