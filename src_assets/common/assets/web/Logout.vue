<template>
  <Navbar-Simple></Navbar-Simple>
  <div class="container py-5">
    <div class="row justify-content-center">
      <div class="col-md-8 col-lg-6">
        <div class="card">
          <div class="card-body text-center">
            <h1 class="h3 mb-3">{{ $t('logout.logged_out') }}</h1>
            <p class="text-muted mb-4">{{ $t('logout.logged_out_desc') }}</p>
            <RouterLink class="btn btn-primary" to="/login">
              <log-in :size="18" class="icon"></log-in>
              {{ $t('logout.login') }}
            </RouterLink>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script>
  import { LogIn } from '@lucide/vue'
  import NavbarSimple from './NavbarSimple.vue'

  export default {
    components: {
      LogIn,
      NavbarSimple,
    },
  }
</script>
