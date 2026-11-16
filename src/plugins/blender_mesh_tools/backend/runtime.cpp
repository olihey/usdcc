// SPDX-License-Identifier: BSD-3-Clause
#include "BKE_global.hh"

// The selected BMesh normal routines inspect G.debug. Own the zero-initialized
// diagnostic state privately; no Blender application scene or Main is created.
namespace blender {
Global G{};
}
