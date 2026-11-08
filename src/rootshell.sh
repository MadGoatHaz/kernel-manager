#!/bin/bash
# rootshell.sh — pkexec privilege escalation wrapper.
#
# Executed by polkit (auth_admin action) to run a shell as root. The kernel-
# manager C++ layer always invokes it via `pkexec "$KM_HELPER_DIR/rootshell.sh"`
# with a single string argument that is the command to run (already quoted and
# validated on the C++ side). We exec a PATH-resolved bash so the script works
# even if bash lives outside /bin (exotic layouts, NixOS, etc.).
#
# This is a leaf: it never spawns its own terminal and never reads from stdin
# (the command string is fully formed by the caller).

exec bash "$@"
