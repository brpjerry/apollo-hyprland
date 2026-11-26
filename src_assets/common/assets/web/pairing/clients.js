/**
 * @file Paired client data: normalizing `GET /api/clients/list` entries and building `POST /api/clients/update`
 * payloads.
 */
import { PERM, sanitizePermissions } from './permissions'

/** @brief Display mode override format: `[Width]x[Height]x[FPS]`, FPS may be fractional. */
export const DISPLAY_MODE_PATTERN = /^\d+x\d+x\d+(\.\d+)?$/

/**
 * @brief Whether a display mode override is empty (auto) or well formed.
 *
 * @param {string} mode Display mode override.
 * @return {boolean} Whether the server will accept it.
 */
export function isValidDisplayMode(mode) {
  const value = (mode ?? '').trim()
  return value === '' || DISPLAY_MODE_PATTERN.test(value)
}

/**
 * @brief Copy a command list into editable `{cmd, elevated}` entries.
 *
 * @param {?Array} commands Commands from the server.
 * @return {{cmd: string, elevated: boolean}[]} Editable commands.
 */
function copyCommands(commands) {
  return (Array.isArray(commands) ? commands : []).map(({ cmd, elevated }) => ({
    cmd: cmd ?? '',
    elevated: elevated === true || elevated === 'true',
  }))
}

/**
 * @brief Normalize one entry of `named_certs` from `GET /api/clients/list`.
 *
 * @param {object} raw Client entry from the server.
 * @return {object} Client with defaults for missing fields.
 */
export function normalizeClient(raw) {
  const perm = Number.parseInt(raw.perm, 10)
  return {
    uuid: raw.uuid,
    name: raw.name ?? '',
    perm: Number.isNaN(perm) ? PERM._no : perm,
    display_mode: raw.display_mode ?? '',
    enabled: raw.enabled !== false,
    connected: raw.connected === true,
    allow_client_commands: raw.allow_client_commands !== false,
    enable_legacy_ordering: raw.enable_legacy_ordering !== false,
    always_use_virtual_display: raw.always_use_virtual_display === true,
    do: copyCommands(raw.do),
    undo: copyCommands(raw.undo),
  }
}

/**
 * @brief Create the editable copy of a client shown in the editor.
 *
 * @param {object} client Normalized client.
 * @return {object} Independent copy whose changes do not touch the list until saved.
 */
export function createClientDraft(client) {
  return {
    ...client,
    do: copyCommands(client.do),
    undo: copyCommands(client.undo),
  }
}

/**
 * @brief Keep the commands that have text, trimmed.
 *
 * @param {{cmd: string, elevated: boolean}[]} commands Edited commands.
 * @return {{cmd: string, elevated: boolean}[]} Commands to save.
 */
function cleanCommands(commands) {
  return commands
    .map(({ cmd, elevated }) => ({ cmd: (cmd ?? '').trim(), elevated: !!elevated }))
    .filter(({ cmd }) => cmd !== '')
}

/**
 * @brief Build the `POST /api/clients/update` body for Apollo's per-client settings.
 *
 * Sending `name` makes the server update every Apollo setting; `enabled` is toggled with its own request.
 *
 * @param {object} draft Edited client.
 * @return {object} Request body.
 */
export function buildClientUpdate(draft) {
  return {
    uuid: draft.uuid,
    name: (draft.name ?? '').trim(),
    display_mode: (draft.display_mode ?? '').trim(),
    allow_client_commands: !!draft.allow_client_commands,
    enable_legacy_ordering: !!draft.enable_legacy_ordering,
    always_use_virtual_display: !!draft.always_use_virtual_display,
    perm: sanitizePermissions(draft.perm),
    do: cleanCommands(draft.do),
    undo: cleanCommands(draft.undo),
  }
}

/**
 * @brief Sort clients by name, unnamed clients last.
 *
 * @param {object[]} clients Normalized clients.
 * @return {object[]} Sorted copy.
 */
export function sortClients(clients) {
  return [...clients].sort((a, b) => {
    if (!a.name !== !b.name) {
      return a.name ? -1 : 1
    }
    return a.name.localeCompare(b.name, undefined, { sensitivity: 'base' })
  })
}
