<template>
  <Navbar-Simple></Navbar-Simple>
  <main id="content" class="container py-5" role="main">
    <div class="row justify-content-center">
      <div class="col-md-8 col-lg-5">
        <div class="card">
          <div class="card-body">
            <h1 class="h3 mb-4 d-flex align-items-center gap-2">
              <img :src="logo" height="45" alt="">
              {{ $t('welcome.greeting') }}
            </h1>
            <form @submit.prevent="login">
              <div class="mb-3">
                <label for="usernameInput" class="form-label">{{ $t('_common.username') }}</label>
                <input type="text" class="form-control" id="usernameInput" autocomplete="username"
                       v-model="credentials.username" required autofocus />
              </div>
              <div class="mb-3">
                <label for="passwordInput" class="form-label">{{ $t('_common.password') }}</label>
                <input type="password" class="form-control" id="passwordInput" autocomplete="current-password"
                       v-model="credentials.password" required />
              </div>
              <button type="submit" class="btn btn-primary w-100" :disabled="loading">
                <log-in :size="18" class="icon"></log-in>
                {{ $t('welcome.login') }}
              </button>
              <div class="alert alert-danger mt-3 mb-0" v-if="error">
                <b>{{ $t('_common.error') }}</b> {{ error }}
              </div>
              <div class="alert alert-success mt-3 mb-0" v-if="success">
                <b>{{ $t('_common.success') }}</b> {{ $t('login.login_success') }}
              </div>
            </form>
          </div>
        </div>
      </div>
    </div>
  </main>
</template>

<script>
  import { LogIn } from '@lucide/vue'
  import NavbarSimple from './NavbarSimple.vue'

  /**
   * @brief localStorage key where Apollo's former "Remember Password" option kept the credentials in clear text.
   *
   * The option is gone (the browser's password manager does that job through the autocomplete fields); the login
   * page only removes what an older version left behind.
   */
  export const SAVED_LOGIN_KEY = 'login'

  /** @brief Placeholder origin used to resolve a return path the way the browser will. */
  const TARGET_BASE = 'http://apollo.invalid'

  /**
   * @brief Pick the page to open after signing in.
   *
   * Only same-site paths are accepted so the `redir` parameter cannot send the browser to another site. The path is
   * resolved as the browser resolves it (tabs and newlines dropped, backslashes and dot segments normalized), and the
   * normalized form is returned, so what was checked is what the browser opens.
   *
   * @param {?string} redir Value of the `redir` query parameter.
   * @return {string} Absolute path inside the Web UI.
   */
  export function loginTarget(redir, hash = '') {
    if (typeof redir !== 'string') {
      return '/'
    }
    let target = redir.startsWith('./') ? redir.substring(1) : redir
    // The server's redirect to the login page cannot carry the fragment in `redir` (browsers never send it), but the
    // browser keeps it on the login page's URL. Put it back, e.g. so /pin#PIN opens the PIN tab.
    if (hash && !target.includes('#')) {
      target += hash
    }
    if (!target.startsWith('/')) {
      return '/'
    }
    let url
    try {
      url = new URL(target, TARGET_BASE)
    } catch {
      return '/'
    }
    const path = url.pathname + url.search + url.hash
    if (url.origin !== TARGET_BASE || path.startsWith('//') || url.pathname === '/login') {
      return '/'
    }
    return path
  }

  export default {
    components: {
      LogIn,
      NavbarSimple,
    },
    data() {
      return {
        credentials: { username: '', password: '' },
        error: null,
        loading: false,
        // Served by the server itself (confighttp's logo route), so it is not bundled.
        logo: '/images/logo-apollo-45.png',
        success: false,
      }
    },
    created() {
      try {
        globalThis.localStorage.removeItem(SAVED_LOGIN_KEY)
      } catch (e) {
        console.error('Removing the saved login failed', e)
      }
    },
    methods: {
      /**
       * @brief Start a cookie login session through `POST /api/login`.
       *
       * @return {Promise<void>} Completion of the login request.
       */
      async login() {
        this.error = null
        this.success = false
        this.loading = true

        let response
        try {
          response = await fetch('./api/login', {
            method: 'POST',
            headers: {
              'Content-Type': 'application/json',
            },
            body: JSON.stringify(this.credentials),
          })
        } catch (e) {
          this.loading = false
          this.error = this.$t('login.login_failed', { error: e.message })
          return
        }
        this.loading = false

        if (response.status === 401) {
          this.error = this.$t('login.wrong_credentials')
          return
        }
        if (response.status !== 200) {
          this.error = this.$t('login.login_failed', { error: response.status })
          return
        }

        this.success = true
        const params = new URLSearchParams(globalThis.location.search)
        globalThis.location.assign(loginTarget(params.get('redir'), globalThis.location.hash))
      },
    },
  }
</script>
