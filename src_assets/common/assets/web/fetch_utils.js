import { notifyKey } from './Notification.vue'

/**
 * The set of error messages that indicate a CSRF validation failure.
 */
const CSRF_ERRORS = new Set(['Missing CSRF token', 'Invalid CSRF token', 'CSRF token expired'])

/** @brief Route of Apollo's session login page. */
export const LOGIN_PATH = '/login'

/** @brief Pages that must not send the browser back to the login page. */
const PUBLIC_PATHS = new Set([LOGIN_PATH, '/logout', '/welcome'])

/**
 * @brief Build the login page URL that returns to the given location after signing in.
 *
 * @param {{pathname: string, search: string, hash: string}} [location] Location to return to.
 * @return {string} Login page URL with a `redir` parameter.
 */
export function loginUrl(location = globalThis.location) {
  const target = `${location.pathname}${location.search}${location.hash}`
  return `${LOGIN_PATH}?redir=${encodeURIComponent(target)}`
}

/**
 * @brief Send the browser to the login page, unless it is already on a public page.
 *
 * @param {Location} [location] Browser location to replace.
 * @return {boolean} Whether a redirect was started.
 */
export function redirectToLogin(location = globalThis.location) {
  if (PUBLIC_PATHS.has(location.pathname.replace(/\/+$/, ''))) {
    return false
  }
  location.assign(loginUrl(location))
  return true
}

/**
 * Wrapper around the native fetch for Web UI API calls.
 *
 * - A 401 response means the login session expired (or the Basic credentials are wrong): the browser goes to the
 *   login page, which returns to the current page after signing in.
 * - A 400 response with a known CSRF error message shows a notification.
 *
 * @param {string} url - The URL to fetch.
 * @param {...RequestInit} options - Standard fetch options.
 * @returns {Promise<Response>} The fetch Response.
 */
export async function apiFetch(url, ...options) {
  const response = await fetch(url, ...options)

  if (response.status === 401) {
    redirectToLogin()
  }

  if (response.status === 400) {
    let body = null
    try {
      body = await response.clone().json()
    } catch (e) {
      console.debug('apiFetch: response body is not JSON', e)
    }

    if (body && CSRF_ERRORS.has(body.error)) {
      notifyKey.error('_common.csrf_error_desc', '_common.csrf_error')
    }
  }

  return response
}

/**
 * @brief Build fetch options that send a JSON body.
 *
 * @param {*} [body] Value to serialize; omitted when undefined.
 * @param {string} [method] HTTP method.
 * @return {RequestInit} Options for apiFetch.
 */
export function jsonRequest(body, method = 'POST') {
  const options = {
    method,
    headers: {
      'Content-Type': 'application/json',
    },
  }
  if (body !== undefined) {
    options.body = JSON.stringify(body)
  }
  return options
}

/**
 * @brief Drop Apollo's login session cookie in this browser.
 *
 * The server sets `auth` without HttpOnly, so the page can expire it. The server keeps the session itself until it
 * expires or the credentials change.
 *
 * @param {Document} [doc] Document whose cookie is cleared.
 */
export function clearSessionCookie(doc = globalThis.document) {
  doc.cookie = 'auth=; Max-Age=0; Path=/; Secure; SameSite=Strict'
}
