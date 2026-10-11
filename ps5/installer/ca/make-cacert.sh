#!/bin/bash
# Builds ca/cacert.pem: Mozilla's CA list as bundled in Node.js (tls.rootCertificates, taken from Mozilla NSS
# for each Node release), without any locally added CA. The installer compiles this list in and checks every
# HTTPS server against it.
set -euo pipefail
cd "$(dirname "$0")"
node -e '
const tls = require("tls");
const certs = tls.rootCertificates;
const head = [
  "# Mozilla CA certificates as bundled in Node.js " + process.version + " (tls.rootCertificates, from Mozilla NSS).",
  "# Made by ca/make-cacert.sh on " + new Date().toISOString().slice(0, 10) + ". " + certs.length + " certificates.",
  ""];
require("fs").writeFileSync("cacert.pem", head.join("\n") + certs.join("\n\n") + "\n");
'
grep -c 'BEGIN CERTIFICATE' cacert.pem
