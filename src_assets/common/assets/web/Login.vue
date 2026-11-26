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
              <div class="mb-3 form-check">
                <input type="checkbox" class="form-check-input" id="savePassword" v-model="savePassword" />
                <label for="savePassword" class="form-check-label">{{ $t('login.save_password') }}</label>
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

  /** @brief localStorage key of the remembered credentials (Apollo's "Remember Password"). */
  export const SAVED_LOGIN_KEY = 'login'

  /**
   * @brief Pick the page to open after signing in.
   *
   * Only same-site paths are accepted so the `redir` parameter cannot send the browser to another site.
   *
   * @param {?string} redir Value of the `redir` query parameter.
   * @return {string} Absolute path inside the Web UI.
   */
  export function loginTarget(redir) {
    if (typeof redir !== 'string') {
      return '/'
    }
    let target = redir.startsWith('./') ? redir.substring(1) : redir
    if (!target.startsWith('/') || target.startsWith('//') || target.startsWith('/\\')) {
      return '/'
    }
    if (target === '/login' || target.startsWith('/login?') || target.startsWith('/login#')) {
      return '/'
    }
    return target
  }

  /**
   * @brief Read the remembered credentials, if any.
   *
   * @param {Storage} storage Browser storage.
   * @return {?{username: string, password: string}} Saved credentials.
   */
  function readSavedLogin(storage) {
    try {
      const saved = storage.getItem(SAVED_LOGIN_KEY)
      if (!saved) {
        return null
      }
      const { username, password } = JSON.parse(saved)
      return { username: username ?? '', password: password ?? '' }
    } catch (e) {
      console.error('Reading the saved login failed', e)
      return null
    }
  }

  export default {
    components: {
      LogIn,
      NavbarSimple,
    },
    data() {
      const saved = readSavedLogin(globalThis.localStorage)
      return {
        credentials: saved ?? { username: '', password: '' },
        error: null,
        loading: false,
        // Served by the server itself (confighttp's logo route), so it is not bundled.
        logo: '/images/logo-apollo-45.png',
        savePassword: saved !== null,
        success: false,
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
        if (!this.savePassword) {
          globalThis.localStorage.removeItem(SAVED_LOGIN_KEY)
        }

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
        if (this.savePassword) {
          globalThis.localStorage.setItem(SAVED_LOGIN_KEY, JSON.stringify(this.credentials))
        }
        const params = new URLSearchParams(globalThis.location.search)
        globalThis.location.assign(loginTarget(params.get('redir')))
      },
    },
  }
</script>
