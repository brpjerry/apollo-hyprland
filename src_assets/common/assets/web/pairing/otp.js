/**
 * @file Helpers for Apollo's one-time-password (OTP) pairing with Artemis.
 */

/** @brief How long a generated PIN stays valid on the server. */
export const OTP_LIFETIME_MS = 3 * 60 * 1000

/** @brief sessionStorage key of a host address the user entered by hand. */
export const HOST_INFO_KEY = 'hostInfo'

/** @brief Moonlight's HTTP port when the Web UI runs on its default port. */
const DEFAULT_STREAM_PORT = 47989

const LOCAL_HOSTNAMES = new Set(['localhost', '127.0.0.1', '[::1]'])

/**
 * @brief Whether the page is opened on the host itself.
 *
 * @param {{hostname: string}} location Browser location.
 * @return {boolean} Whether the hostname is a loopback name.
 */
export function isLocalHost(location) {
  return LOCAL_HOSTNAMES.has(location.hostname)
}

/**
 * @brief Build the Artemis deep link that pairs with the generated PIN.
 *
 * An IPv6 address gets the brackets an authority needs, and each query value is encoded on its own so a host
 * name with `#` or `&` stays inside its value.
 *
 * @param {{hostAddr: string, hostPort: (string|number), otp: string, passphrase: string, hostName: string}} info
 * @return {string} `art://` URL.
 */
export function buildDeepLink({ hostAddr, hostPort, otp, passphrase, hostName }) {
  const host = hostAddr.includes(':') && !hostAddr.startsWith('[') ? `[${hostAddr}]` : hostAddr
  const query = [['pin', otp], ['passphrase', passphrase], ['name', hostName]]
    .map(([key, value]) => `${key}=${encodeURIComponent(value ?? '')}`)
    .join('&')
  return `art://${host}:${hostPort}?${query}`
}

/**
 * @brief Pick the address Artemis should connect to.
 *
 * The Web UI port is Moonlight's port + 1. When the page is opened remotely, the address used to reach it is
 * the best guess; on the host itself the server reports its LAN address.
 *
 * @param {string} serverIp Address reported by `POST /api/otp`.
 * @param {{hostname: string, port: string}} location Browser location.
 * @return {{hostAddr: string, hostPort: number}} Host address and port.
 */
export function guessHost(serverIp, location) {
  const webPort = Number.parseInt(location.port, 10)
  return {
    hostAddr: isLocalHost(location) ? serverIp : location.hostname,
    hostPort: Number.isNaN(webPort) ? DEFAULT_STREAM_PORT : webPort - 1,
  }
}

/**
 * @brief Read the host address the user entered by hand in this browser session.
 *
 * A browser that denies storage access (even reading `sessionStorage` can throw) just has no saved host.
 *
 * @param {Storage} [storage] Storage to read instead of the session storage.
 * @return {?{hostAddr: string, hostPort: (string|number)}} Saved host, if any.
 */
export function readSavedHost(storage) {
  try {
    const saved = JSON.parse((storage ?? globalThis.sessionStorage).getItem(HOST_INFO_KEY))
    return saved && saved.hostAddr ? { hostAddr: saved.hostAddr, hostPort: saved.hostPort } : null
  } catch {
    return null
  }
}

/**
 * @brief Remember a host address entered by hand for this browser session, where the browser allows it.
 *
 * Pairing only needs the address on the current page, so a storage that can't be used is not an error.
 *
 * @param {{hostAddr: string, hostPort: (string|number)}} host Host to save.
 * @param {Storage} [storage] Storage to write instead of the session storage.
 * @return {boolean} Whether the host was saved.
 */
export function saveHost({ hostAddr, hostPort }, storage) {
  try {
    const target = storage ?? globalThis.sessionStorage
    target.setItem(HOST_INFO_KEY, JSON.stringify({ hostAddr, hostPort }))
    return true
  } catch (e) {
    console.warn('Could not remember the pairing host', e)
    return false
  }
}

let qrCodeLoader = null

/**
 * @brief Load the QR code library shipped in `assets/js/qrcode.min.js` (it defines the global `QRCode`).
 *
 * @param {Document} [doc] Document to add the script to.
 * @return {Promise<Function>} The QRCode constructor.
 */
export function loadQRCode(doc = globalThis.document) {
  if (globalThis.QRCode) {
    return Promise.resolve(globalThis.QRCode)
  }
  qrCodeLoader ??= new Promise((resolve, reject) => {
    const script = doc.createElement('script')
    script.src = '/assets/js/qrcode.min.js'
    script.onload = () => resolve(globalThis.QRCode)
    script.onerror = () => {
      qrCodeLoader = null
      reject(new Error('Failed to load the QR code library'))
    }
    doc.head.appendChild(script)
  })
  return qrCodeLoader
}
