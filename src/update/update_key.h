#pragma once

/*
 * The public half of Cobalt's release signing key: 32 bytes of Ed25519, as 64
 * hex characters. The private half exists only as the GitHub Actions secret
 * UPDATE_SIGNING_KEY, which .github/workflows/sign-release.yml uses to sign each
 * release's update.json. Replacing this key means shipping a build that carries
 * the new one, signed by the old one; a client only trusts the key it has.
 */
#define COBALT_UPDATE_PUBLIC_KEY_HEX \
   "a6a2bc322f6de6ba94832a261719ec1a331d9111381a6dd2f86762f63036d160"
