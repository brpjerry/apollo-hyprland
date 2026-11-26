/**
 * @file Client permission bitmask, mirroring `crypto::PERM` in `src/crypto.h`.
 */

/** @brief Permission bits by name. */
export const PERM = Object.freeze({
  // Input permission group
  input_controller: 0x00000100,
  input_touch: 0x00000200,
  input_pen: 0x00000400,
  input_mouse: 0x00000800,
  input_kbd: 0x00001000,
  _all_inputs: 0x00001F00,

  // Operation permission group
  clipboard_set: 0x00010000,
  clipboard_read: 0x00020000,
  file_upload: 0x00040000,
  file_dwnload: 0x00080000,
  server_cmd: 0x00100000,
  _all_operations: 0x001F0000,

  // Action permission group
  list: 0x01000000,
  view: 0x02000000,
  launch: 0x04000000,
  _allow_view: 0x06000000,
  _all_actions: 0x07000000,

  // Special values
  _default: 0x03000000,
  _no: 0x00000000,
  _all: 0x071F1F00,
})

/**
 * @brief Permissions shown in the client editor, by group.
 *
 * A permission listed in `suppressedBy` is implied by those permissions: Launch implies View and List, View implies
 * List. File upload and download are not offered, as in Apollo.
 */
export const PERMISSION_GROUPS = Object.freeze([
  {
    name: 'Action',
    permissions: [
      { name: 'list', suppressedBy: ['view', 'launch'] },
      { name: 'view', suppressedBy: ['launch'] },
      { name: 'launch', suppressedBy: [] },
    ],
  },
  {
    name: 'Operation',
    permissions: [
      { name: 'clipboard_set', suppressedBy: [] },
      { name: 'clipboard_read', suppressedBy: [] },
      { name: 'server_cmd', suppressedBy: [] },
    ],
  },
  {
    name: 'Input',
    permissions: [
      { name: 'input_controller', suppressedBy: [] },
      { name: 'input_touch', suppressedBy: [] },
      { name: 'input_pen', suppressedBy: [] },
      { name: 'input_mouse', suppressedBy: [] },
      { name: 'input_kbd', suppressedBy: [] },
    ],
  },
])

/**
 * @brief Whether a permission bit is set.
 *
 * @param {number} perm Permission bitmask.
 * @param {string} name Permission name from PERM.
 * @return {boolean} Whether the permission is granted.
 */
export function hasPermission(perm, name) {
  return (perm & PERM[name]) !== 0
}

/**
 * @brief Whether a permission is implied by a broader one that is granted.
 *
 * @param {number} perm Permission bitmask.
 * @param {string[]} suppressedBy Permissions that imply this one.
 * @return {boolean} Whether one of them is granted.
 */
export function isSuppressed(perm, suppressedBy) {
  return suppressedBy.some(name => hasPermission(perm, name))
}

/**
 * @brief Flip one permission bit.
 *
 * @param {number} perm Permission bitmask.
 * @param {string} name Permission name from PERM.
 * @return {number} Updated bitmask.
 */
export function togglePermission(perm, name) {
  return (perm ^ PERM[name]) >>> 0
}

/**
 * @brief Keep only the bits the server knows about.
 *
 * @param {number} perm Permission bitmask.
 * @return {number} Bitmask restricted to PERM._all.
 */
export function sanitizePermissions(perm) {
  return (perm & PERM._all) >>> 0
}

/**
 * @brief Whether the client may start apps, which Apollo highlights as a powerful permission.
 *
 * @param {number} perm Permission bitmask.
 * @return {boolean} Whether Launch (or anything above it) is granted.
 */
export function isPowerful(perm) {
  return perm >= PERM.launch
}

/**
 * @brief Format a bitmask as Apollo's three hex bytes: actions, operations, inputs.
 *
 * @param {number} perm Permission bitmask.
 * @return {string} For example `07 1F 1F`.
 */
export function permToStr(perm) {
  return [(perm >> 24) & 0xFF, (perm >> 16) & 0xFF, (perm >> 8) & 0xFF]
    .map(segment => segment.toString(16).toUpperCase().padStart(2, '0'))
    .join(' ')
}
