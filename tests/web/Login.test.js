import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

vi.mock('../../src_assets/common/assets/web/Notification.vue', () => ({
  default: { template: '<div />' },
  notifyKey: { error: vi.fn() },
}))
vi.mock('../../src_assets/common/assets/web/NavbarSimple.vue', () => ({
  default: { template: '<div />' },
}))

import { notifyKey } from '../../src_assets/common/assets/web/Notification.vue'
import { apiFetch, clearSessionCookie, loginUrl, redirectToLogin } from '../../src_assets/common/assets/web/fetch_utils'
import Login, { loginTarget, SAVED_LOGIN_KEY } from '../../src_assets/common/assets/web/Login.vue'

/**
 * @brief Build a stand-in for window.location.
 *
 * @param {string} pathname Current path.
 * @param {string} [search] Query string.
 * @param {string} [hash] Fragment.
 * @return {object} Location with a spied assign().
 */
function fakeLocation(pathname, search = '', hash = '') {
  return { pathname, search, hash, hostname: 'localhost', assign: vi.fn() }
}

beforeEach(() => {
  vi.clearAllMocks()
  localStorage.clear()
})

afterEach(() => {
  vi.unstubAllGlobals()
})

describe('login redirect on 401', () => {
  // Bugs caught: 0, False positives: 0
  it('sends an unauthorized API call to the login page with the current page as redir', async () => {
    const location = fakeLocation('/config', '?tab=1', '#av')
    vi.stubGlobal('location', location)
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue({ status: 401 }))

    const response = await apiFetch('./api/config')

    expect(response.status).toBe(401)
    expect(fetch).toHaveBeenCalledExactlyOnceWith('./api/config')
    expect(location.assign).toHaveBeenCalledExactlyOnceWith('/login?redir=%2Fconfig%3Ftab%3D1%23av')
  })

  // Bugs caught: 0, False positives: 0
  it.each(['/login', '/logout', '/welcome', '/login/'])('stays on the public page %s', pathname => {
    const location = fakeLocation(pathname)

    expect(redirectToLogin(location)).toBe(false)
    expect(location.assign).not.toHaveBeenCalled()
  })

  // Bugs caught: 0, False positives: 0
  it('leaves successful calls alone and still reports CSRF failures', async () => {
    const location = fakeLocation('/apps')
    vi.stubGlobal('location', location)
    vi.stubGlobal('fetch', vi.fn()
      .mockResolvedValueOnce({ status: 200 })
      .mockResolvedValueOnce({ status: 400, clone: () => ({ json: async () => ({ error: 'Invalid CSRF token' }) }) }))

    await apiFetch('./api/apps')
    await apiFetch('./api/apps', { method: 'POST' })

    expect(location.assign).not.toHaveBeenCalled()
    expect(notifyKey.error).toHaveBeenCalledOnce()
  })

  // Bugs caught: 0, False positives: 0
  it('builds the login URL from the path, query and hash', () => {
    expect(loginUrl(fakeLocation('/pin', '', '#PIN'))).toBe('/login?redir=%2Fpin%23PIN')
  })

  // Bugs caught: 0, False positives: 0
  it('expires the session cookie on logout', () => {
    const doc = { cookie: 'auth=abc' }

    clearSessionCookie(doc)

    expect(doc.cookie).toBe('auth=; Max-Age=0; Path=/; Secure; SameSite=Strict')
  })
})

describe('login target', () => {
  // Bugs caught: 0, False positives: 0
  it.each([
    [null, '/'],
    ['/apps', '/apps'],
    ['/config?x=1#av', '/config?x=1#av'],
    ['./pin', '/pin'],
    ['https://evil.example/', '/'],
    ['//evil.example/', '/'],
    ['/\\evil.example', '/'],
    ['javascript:alert(1)', '/'],
    ['/login?redir=%2Fapps', '/'],
  ])('maps redir %s to %s', (redir, target) => {
    expect(loginTarget(redir)).toBe(target)
  })
})

describe('Login page', () => {
  /**
   * @brief Build the component state used by login().
   *
   * @return {object} Context for Login.methods.login.
   */
  function loginContext() {
    return { ...Login.data(), $t: (key, params) => (params ? `${key}:${JSON.stringify(params)}` : key) }
  }

  // Bugs caught: 0, False positives: 0
  it('posts the credentials to /api/login and opens the requested page', async () => {
    const location = fakeLocation('/login', '?redir=%2Fapps')
    vi.stubGlobal('location', location)
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue({ status: 200 }))
    const ctx = loginContext()
    ctx.credentials = { username: 'apollo', password: 'secret' }

    await Login.methods.login.call(ctx)

    expect(fetch).toHaveBeenCalledExactlyOnceWith('./api/login', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ username: 'apollo', password: 'secret' }),
    })
    expect(ctx.success).toBe(true)
    expect(ctx.error).toBeNull()
    expect(location.assign).toHaveBeenCalledExactlyOnceWith('/apps')
    expect(localStorage.getItem(SAVED_LOGIN_KEY)).toBeNull()
  })

  // Bugs caught: 0, False positives: 0
  it('shows wrong credentials on 401 and stays on the page', async () => {
    const location = fakeLocation('/login')
    vi.stubGlobal('location', location)
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue({ status: 401 }))
    const ctx = loginContext()

    await Login.methods.login.call(ctx)

    expect(ctx.error).toBe('login.wrong_credentials')
    expect(ctx.loading).toBe(false)
    expect(location.assign).not.toHaveBeenCalled()
  })

  // Bugs caught: 0, False positives: 0
  it('remembers the credentials when asked and fills them in next time', async () => {
    vi.stubGlobal('location', fakeLocation('/login'))
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue({ status: 200 }))
    const ctx = loginContext()
    ctx.credentials = { username: 'apollo', password: 'secret' }
    ctx.savePassword = true

    await Login.methods.login.call(ctx)

    const next = Login.data()
    expect(next.savePassword).toBe(true)
    expect(next.credentials).toEqual({ username: 'apollo', password: 'secret' })
  })

  // Bugs caught: 0, False positives: 0
  it('reports network failures', async () => {
    vi.stubGlobal('location', fakeLocation('/login'))
    vi.stubGlobal('fetch', vi.fn().mockRejectedValue(new Error('offline')))
    const ctx = loginContext()

    await Login.methods.login.call(ctx)

    expect(ctx.error).toBe('login.login_failed:{"error":"offline"}')
    expect(ctx.loading).toBe(false)
  })
})
