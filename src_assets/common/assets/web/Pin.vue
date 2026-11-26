<template>
  <Navbar></Navbar>
  <div id="content" class="container d-flex flex-column align-items-center">
    <ul class="nav nav-pills my-4 justify-content-center" role="tablist">
      <li class="nav-item" role="presentation">
        <button type="button" class="nav-link" role="tab" :class="{ active: currentTab === 'OTP' }"
                :aria-selected="currentTab === 'OTP'" @click="currentTab = 'OTP'">
          <qr-code :size="18" class="icon"></qr-code>
          {{ $t('pin.otp_pairing') }}
        </button>
      </li>
      <li class="nav-item" role="presentation">
        <button type="button" class="nav-link" role="tab" :class="{ active: currentTab === 'PIN' }"
                :aria-selected="currentTab === 'PIN'" @click="currentTab = 'PIN'">
          <hash :size="18" class="icon"></hash>
          {{ $t('pin.pin_pairing') }}
        </button>
      </li>
    </ul>
    <OtpPairing v-if="currentTab === 'OTP'"></OtpPairing>
    <form v-else class="form d-flex flex-column align-items-center" id="form" @submit.prevent="registerDevice">
      <div class="card flex-column d-flex p-4 mb-4">
        <div class="input-group mt-2">
          <label for="pairing-input" class="visually-hidden">{{ $t('pin.select_pairing') }}</label>
          <span class="input-group-text">
            <user-round-search :size="18" class="icon"></user-round-search>
          </span>
          <select v-model="selectedPairingId" class="form-select" id="pairing-input" required>
            <option disabled value="">
              {{ pendingPairings.length ? $t('pin.select_pairing') : $t('pin.no_pending_pairings') }}
            </option>
            <option v-for="pairing in pendingPairings" :key="pairing.id" :value="pairing.id">
              {{ pairing.name || $t('pin.unknown_device') }} — {{ pairing.address || $t('pin.unknown_address') }}
            </option>
          </select>
          <button
            type="button"
            class="btn btn-outline-danger"
            :disabled="!selectedPairingId"
            :title="$t('pin.cancel_pairing')"
            @click="cancelSelectedPairing"
          >
            <x :size="18" class="icon"></x>
          </button>
        </div>
        <div class="input-group mt-2">
          <label for="pin-input" class="visually-hidden">{{ $t('navbar.pin') }}</label>
          <span class="input-group-text">
            <hash :size="18" class="icon"></hash>
          </span>
          <input v-model="pin" type="text" pattern="\d{4}" maxlength="4" inputmode="numeric" :placeholder="`${$t('navbar.pin')}`" id="pin-input" class="form-control" required />
        </div>
        <div class="input-group my-4">
          <label for="name-input" class="visually-hidden">{{ $t('pin.device_name') }}</label>
          <span class="input-group-text">
            <monitor :size="18" class="icon"></monitor>
          </span>
          <input v-model="name" type="text" maxlength="128" :placeholder="namePlaceholder" id="name-input" class="form-control" />
        </div>
        <button type="submit" class="btn btn-primary">
          <forward :size="18" class="icon"></forward>
          {{ $t('pin.send') }}
        </button>
      </div>
      <div v-if="status" :class="`alert alert-${status.type}`" role="alert">{{ status.message }}</div>
    </form>
    <div class="alert alert-warning">
      <b>{{ $t('_common.warning') }}</b> {{ $t('pin.warning_msg') }}
    </div>
    <ClientManager ref="clientManager" class="w-100"></ClientManager>
  </div>
</template>

<script>
  import Navbar from './Navbar.vue'
  import { apiFetch } from './fetch_utils'
  import ClientManager from './pairing/ClientManager.vue'
  import OtpPairing from './pairing/OtpPairing.vue'
  import {
    Forward,
    Hash,
    Monitor,
    QrCode,
    UserRoundSearch,
    X,
  } from '@lucide/vue'

  /** @brief Delay before reloading the client list, so a new pairing is saved first. */
  const CLIENT_REFRESH_DELAY_MS = 1000

  export default {
    components: {
      ClientManager,
      Navbar,
      OtpPairing,
      Forward,
      Hash,
      Monitor,
      QrCode,
      UserRoundSearch,
      X,
    },
    inject: ['i18n'],
    data() {
      return {
        // Apollo opens on OTP pairing; `#PIN` in the address opens PIN pairing.
        currentTab: globalThis.location?.hash?.toUpperCase() === '#PIN' ? 'PIN' : 'OTP',
        name: '',
        pendingPairings: [],
        pin: '',
        refreshTimer: null,
        selectedPairingId: '',
        status: null,
      };
    },
    mounted() {
      this.loadPendingPairings();
      this.refreshTimer = window.setInterval(() => this.loadPendingPairings(), 2000);
    },
    beforeUnmount() {
      window.clearInterval(this.refreshTimer);
      window.clearTimeout(this.clientRefreshTimer);
    },
    computed: {
      /**
       * Without a name the server keeps the name the client reported, so show that name as the placeholder.
       */
      namePlaceholder() {
        const pairing = this.pendingPairings.find((p) => p.id === this.selectedPairingId);
        return pairing?.name ? `${this.$t('pin.device_name')} (${pairing.name})` : this.$t('pin.device_name');
      },
    },
    methods: {
      /**
       * Refresh the authenticated list of pairing requests awaiting approval.
       */
      async loadPendingPairings() {
        try {
          const response = await apiFetch('./api/pin', {method: 'GET'});
          if (!response.ok) {
            throw new Error(`HTTP ${response.status}`);
          }

          const body = await response.json();
          this.pendingPairings = body.pairings || [];
          if (!this.pendingPairings.some((pairing) => pairing.id === this.selectedPairingId)) {
            this.selectedPairingId = this.pendingPairings.length === 1 ? this.pendingPairings[0].id : '';
          }
        } catch (error) {
          console.error('Failed to load pending pairing requests', error);
          this.status = {type: 'danger', message: this.i18n.t('pin.load_failure')};
        }
      },

      /**
       * Apply the entered PIN only to the pairing request selected by the operator.
       */
      async registerDevice() {
        this.status = null;
        if (!this.selectedPairingId) {
          this.status = {type: 'danger', message: this.i18n.t('pin.select_pairing_required')};
          return;
        }

        const body = JSON.stringify({
          pairing_id: this.selectedPairingId,
          pin: this.pin,
          name: this.name.trim(),
        });
        const response = await apiFetch('./api/pin', {
          method: "POST",
          headers: {
            'Content-Type': 'application/json'
          },
          body,
        });
        const result = await response.json();
        if (result.status === true) {
          this.status = {type: 'success', message: this.i18n.t('pin.pair_success_check_perm')};
          this.pin = '';
          this.name = '';
          window.clearTimeout(this.clientRefreshTimer);
          this.clientRefreshTimer = window.setTimeout(() => this.$refs.clientManager?.refreshClients(), CLIENT_REFRESH_DELAY_MS);
        } else {
          this.status = {type: 'danger', message: this.i18n.t('pin.pair_failure')};
        }
        await this.loadPendingPairings();
      },

      /**
       * Cancel the selected pending request without affecting other clients.
       */
      async cancelSelectedPairing() {
        if (!this.selectedPairingId) {
          return;
        }

        const response = await apiFetch('./api/pin', {
          method: 'DELETE',
          headers: {
            'Content-Type': 'application/json',
          },
          body: JSON.stringify({pairing_id: this.selectedPairingId}),
        });
        const result = await response.json();
        this.status = result.status === true
          ? {type: 'success', message: this.i18n.t('pin.cancel_success')}
          : {type: 'danger', message: this.i18n.t('pin.cancel_failure')};
        await this.loadPendingPairings();
      }
    }
  }
</script>
