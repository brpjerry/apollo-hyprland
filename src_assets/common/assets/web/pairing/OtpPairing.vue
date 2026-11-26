<template>
  <form class="form d-flex flex-column align-items-center" @submit.prevent="requestOTP">
    <div class="card flex-column d-flex p-4 mb-4 otp-card">
      <div v-show="showLink" ref="qrContainer" class="qr-container mb-2 p-2 bg-white align-self-center"></div>
      <p v-if="showLink" class="text-center text-secondary d-flex align-items-center justify-content-center gap-2">
        <a class="text-secondary" :href="deepLink">art://{{ hostAddr }}:{{ hostPort }}</a>
        <button type="button" class="btn btn-link btn-sm p-0 text-secondary" :title="$t('pin.otp_edit_host')" @click="editHost">
          <pencil :size="16" class="icon"></pencil>
        </button>
      </p>
      <h1 class="mb-4 text-center otp-display">{{ otp || '????' }}</h1>
      <div v-if="editingHost" class="d-flex flex-column align-items-stretch">
        <label for="otp-host" class="visually-hidden">{{ $t('pin.otp_host') }}</label>
        <input id="otp-host" type="text" :placeholder="$t('pin.otp_host')" v-model="hostAddr" class="form-control mt-2" />
        <label for="otp-port" class="visually-hidden">{{ $t('pin.otp_port') }}</label>
        <input id="otp-port" type="text" :placeholder="$t('pin.otp_port')" v-model="hostPort" class="form-control my-4" />
        <button type="button" class="btn btn-primary" :disabled="!canSaveHost" @click="saveHost">
          <save :size="18" class="icon"></save>
          {{ $t('_common.save') }}
        </button>
      </div>
      <div v-else class="d-flex flex-column align-items-stretch">
        <div class="input-group mt-2">
          <label for="otp-passphrase" class="visually-hidden">{{ $t('pin.otp_passphrase') }}</label>
          <span class="input-group-text"><key-round :size="18" class="icon"></key-round></span>
          <input id="otp-passphrase" type="text" pattern="[0-9a-zA-Z]{4,}" :placeholder="$t('pin.otp_passphrase')"
                 v-model="passphrase" required class="form-control" />
        </div>
        <div class="input-group my-4">
          <label for="otp-device-name" class="visually-hidden">{{ $t('pin.device_name') }}</label>
          <span class="input-group-text"><monitor :size="18" class="icon"></monitor></span>
          <input id="otp-device-name" type="text" :placeholder="$t('pin.device_name')" v-model="deviceName" class="form-control" />
        </div>
        <button type="submit" class="btn btn-primary" :disabled="requesting">
          <qr-code :size="18" class="icon"></qr-code>
          {{ $t('pin.generate_pin') }}
        </button>
      </div>
    </div>
    <div v-if="otpMessage" class="alert" :class="`alert-${otpStatus}`">{{ otpMessage }}</div>
    <div class="alert alert-info">{{ $t('pin.otp_msg') }}</div>
  </form>
</template>

<script>
  import { KeyRound, Monitor, Pencil, QrCode, Save } from '@lucide/vue'
  import { apiFetch, jsonRequest } from '../fetch_utils'
  import {
    OTP_LIFETIME_MS,
    buildDeepLink,
    guessHost,
    isLocalHost,
    loadQRCode,
    readSavedHost,
    saveHost,
  } from './otp'

  export default {
    components: { KeyRound, Monitor, Pencil, QrCode, Save },
    inject: ['i18n'],
    data() {
      return {
        deviceName: '',
        editingHost: false,
        expired: false,
        hostAddr: '',
        hostName: '',
        hostPort: '',
        issuedPassphrase: '',
        otp: '',
        otpMessage: '',
        otpStatus: 'warning',
        passphrase: '',
        requesting: false,
      }
    },
    computed: {
      /** The link carries the passphrase the PIN was issued for, not whatever the field holds now. */
      deepLink() {
        const { hostAddr, hostPort, otp, hostName, issuedPassphrase } = this
        return buildDeepLink({ hostAddr, hostPort, otp, hostName, passphrase: issuedPassphrase })
      },
      canSaveHost() {
        return !!(this.hostAddr && this.hostPort)
      },
      showLink() {
        return this.editingHost || !!(this.otp && this.hostAddr && !this.expired)
      },
    },
    beforeUnmount() {
      clearTimeout(this.expiryTimer)
    },
    methods: {
      /**
       * @brief Ask the server for a PIN that pairs the client presenting this passphrase.
       *
       * @return {Promise<void>} Completion of the request.
       */
      async requestOTP() {
        if (this.editingHost) {
          return
        }
        this.requesting = true
        const passphrase = this.passphrase
        let body
        try {
          const response = await apiFetch('./api/otp', jsonRequest({ passphrase, deviceName: this.deviceName }))
          body = await response.json()
        } catch (e) {
          body = { status: false, error: e.message }
        } finally {
          this.requesting = false
        }

        if (!body.status) {
          this.otpMessage = body.error || body.message
          this.otpStatus = 'danger'
          return
        }

        this.expired = false
        this.otp = body.otp
        this.issuedPassphrase = passphrase
        this.hostName = body.name
        this.otpStatus = 'success'
        this.otpMessage = this.i18n.t('pin.otp_success')
        Object.assign(this, readSavedHost(globalThis.sessionStorage) ?? guessHost(body.ip, globalThis.location))
        // Without an address there is no link, and the editor is the only way to enter one.
        this.editingHost = !this.hostAddr

        clearTimeout(this.expiryTimer)
        this.expiryTimer = setTimeout(() => this.expire(), OTP_LIFETIME_MS)

        if (this.hostAddr) {
          await this.renderQR()
          if (!isLocalHost(globalThis.location)) {
            setTimeout(() => {
              if (globalThis.confirm(this.i18n.t('pin.otp_pair_now'))) {
                globalThis.open(this.deepLink)
              }
            }, 0)
          }
        }
      },
      /** @brief Forget the PIN once the server no longer accepts it. */
      expire() {
        this.expired = true
        this.editingHost = false
        this.otp = this.i18n.t('pin.otp_expired')
        this.otpMessage = this.i18n.t('pin.otp_expired_msg')
        this.otpStatus = 'warning'
        this.passphrase = ''
      },
      editHost() {
        this.editingHost = !this.editingHost
      },
      async saveHost() {
        if (!this.canSaveHost) {
          return
        }
        saveHost(globalThis.sessionStorage, this)
        this.editingHost = false
        await this.renderQR()
      },
      /** @brief Draw the deep link as a QR code for scanning with Artemis. */
      async renderQR() {
        await this.$nextTick()
        const container = this.$refs.qrContainer
        if (!container) {
          return
        }
        try {
          const QRCode = await loadQRCode()
          container.replaceChildren()
          new QRCode(container, this.deepLink)
        } catch (e) {
          console.error(e)
        }
      },
    },
  }
</script>

<style scoped>
.otp-card {
  min-width: 18rem;
}

.otp-display {
  font-variant-numeric: tabular-nums;
  letter-spacing: 0.2em;
}

.qr-container :deep(img),
.qr-container :deep(canvas) {
  display: block;
}
</style>
