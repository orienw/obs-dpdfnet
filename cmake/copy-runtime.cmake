# SPDX-License-Identifier: GPL-2.0-or-later

# Several targets copy the same runtime libraries into one output directory
# from parallel post-build steps, so take turns.
file(LOCK "${DESTINATION}/.dpdfnet-runtime-copy.lock" GUARD PROCESS
  TIMEOUT 120)
file(COPY "${SOURCE}" DESTINATION "${DESTINATION}" FOLLOW_SYMLINK_CHAIN)
