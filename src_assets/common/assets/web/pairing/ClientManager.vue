<template>
  <div class="card my-4 align-self-stretch" id="clients">
    <div class="card-body">
      <div class="d-flex justify-content-between align-items-center mb-3 gap-2">
        <h2 id="unpair" class="mb-0">{{ $t('pin.device_management') }}</h2>
        <button type="button" class="btn btn-danger" :disabled="unpairAllPressed || clients.length === 0" @click="unpairAll">
          <trash-2 :size="18" class="icon"></trash-2>
          {{ $t('pin.unpair_all') }}
        </button>
      </div>
      <p class="mb-0">{{ $t('pin.device_management_desc') }}</p>
      <p class="mb-0">
        {{ $t('pin.device_management_warning') }}
        <a :href="permissionWiki" target="_blank" rel="noopener noreferrer">{{ $t('_common.learn_more') }}</a>
      </p>
      <div class="alert alert-success d-flex align-items-center mt-3" v-if="showUnpairMessage">
        <check-circle :size="18" class="icon"></check-circle>
        <div><b>{{ $t('_common.success') }}</b> {{ $t('pin.unpair_single_success') }}</div>
        <button type="button" class="btn btn-success ms-auto" @click="showUnpairMessage = false">{{ $t('_common.dismiss') }}</button>
      </div>
      <div class="alert alert-success mt-3" v-if="unpairAllStatus === true">
        {{ $t('pin.unpair_all_success') }}
      </div>
      <div class="alert alert-danger mt-3" v-if="unpairAllStatus === false">
        {{ $t('pin.unpair_all_error') }}
      </div>
      <div class="alert alert-danger mt-3" v-if="saveError">
        {{ $t('pin.save_client_error') }} {{ saveError }}
      </div>
    </div>
    <ul class="list-group list-group-flush" v-if="clients.length > 0">
      <li v-for="client in clients" :key="client.uuid" class="list-group-item">
        <ClientEditor v-if="draft && draft.uuid === client.uuid"
                      :draft="draft"
                      :platform="platform"
                      :busy="saving"
                      @save="saveClient"
                      @cancel="cancelEdit"></ClientEditor>
        <div v-else class="d-flex align-items-center gap-2 client-row">
          <span class="badge" :class="isPowerful(client.perm) ? 'bg-danger' : 'bg-primary'">[ {{ permToStr(client.perm) }} ]</span>
          <span class="flex-grow-1" :class="{ 'text-muted': !client.enabled }">
            {{ client.name !== '' ? client.name : $t('pin.unpair_single_unknown') }}
            <span v-if="client.connected" class="badge bg-success ms-1">{{ $t('pin.client_connected') }}</span>
          </span>
          <div class="form-check form-switch mb-0" :title="$t('pin.client_enabled')">
            <input class="form-check-input" type="checkbox" role="switch"
                   :id="'toggle-' + client.uuid"
                   :checked="client.enabled"
                   :aria-label="$t('pin.client_enabled')"
                   @change="toggleClient(client, !client.enabled)">
          </div>
          <button v-if="client.connected" type="button" class="btn btn-warning" :title="$t('pin.disconnect_client')"
                  @click="disconnectClient(client)">
            <unplug :size="18" class="icon"></unplug>
          </button>
          <button type="button" class="btn btn-primary" :title="$t('pin.edit_client')" @click="editClient(client)">
            <pencil :size="18" class="icon"></pencil>
          </button>
          <button type="button" class="btn btn-danger" :title="$t('pin.unpair_client')" @click="unpairSingle(client)">
            <trash-2 :size="18" class="icon"></trash-2>
          </button>
        </div>
      </li>
    </ul>
    <ul v-else class="list-group list-group-flush">
      <li class="list-group-item p-3 text-center"><em>{{ $t('pin.unpair_single_no_devices') }}</em></li>
    </ul>
  </div>
</template>

<script>
  import { CheckCircle, Pencil, Trash2, Unplug } from '@lucide/vue'
  import { apiFetch, jsonRequest } from '../fetch_utils'
  import { apolloWikiPage } from '../links'
  import ClientEditor from './ClientEditor.vue'
  import { buildClientUpdate, createClientDraft, normalizeClient, sortClients } from './clients'
  import { isPowerful, permToStr } from './permissions'

  export default {
    components: { CheckCircle, ClientEditor, Pencil, Trash2, Unplug },
    data() {
      return {
        clients: [],
        draft: null,
        permissionWiki: apolloWikiPage('Permission-System'),
        platform: '',
        saveError: '',
        saving: false,
        showUnpairMessage: false,
        unpairAllPressed: false,
        unpairAllStatus: null,
      }
    },
    created() {
      this.refreshClients()
    },
    beforeUnmount() {
      clearTimeout(this.unpairAllTimer)
      clearTimeout(this.disconnectTimer)
    },
    methods: {
      isPowerful,
      permToStr,
      /**
       * @brief Reload the paired clients.
       *
       * An open editor keeps its changes unless its client is gone. A failed request keeps the last list.
       *
       * @return {Promise<void>} Completion of the request.
       */
      async refreshClients() {
        try {
          const response = await apiFetch('./api/clients/list')
          const body = await response.json()
          this.platform = body.platform ?? this.platform
          if (body.status && Array.isArray(body.named_certs)) {
            this.clients = sortClients(body.named_certs.map(normalizeClient))
            if (this.draft && !this.clients.some(client => client.uuid === this.draft.uuid)) {
              this.draft = null
            }
          }
        } catch (e) {
          console.error('Failed to load paired clients', e)
        }
      },
      editClient(client) {
        this.saveError = ''
        this.draft = createClientDraft(client)
      },
      cancelEdit() {
        this.draft = null
      },
      /**
       * @brief Save Apollo's per-client settings. The editor closes on success and stays open, with the changes,
       * when the save fails.
       *
       * @param {object} draft Edited client.
       * @return {Promise<void>} Completion of the save and refresh.
       */
      async saveClient(draft) {
        this.saving = true
        this.saveError = ''
        try {
          const response = await apiFetch('./api/clients/update', jsonRequest(buildClientUpdate(draft)))
          const body = await response.json()
          if (!body.status) {
            throw new Error(body.error || body.message || `HTTP ${response.status}`)
          }
          if (this.draft === draft) {
            this.draft = null
          }
        } catch (e) {
          this.saveError = e.message
          return
        } finally {
          this.saving = false
        }
        await this.refreshClients()
      },
      /**
       * @brief Enable or disable a client (Sunshine's switch). Disabling ends its stream.
       *
       * @param {object} client Client to change.
       * @param {boolean} enabled New state.
       * @return {Promise<void>} Completion of the update and refresh.
       */
      async toggleClient(client, enabled) {
        try {
          await apiFetch('./api/clients/update', jsonRequest({ uuid: client.uuid, enabled }))
        } catch (e) {
          console.error('Failed to change the client state', e)
        }
        await this.refreshClients()
      },
      async disconnectClient(client) {
        try {
          await apiFetch('./api/clients/disconnect', jsonRequest({ uuid: client.uuid }))
        } catch (e) {
          console.error('Failed to disconnect the client', e)
        }
        await this.refreshClients()
        // The stream takes a moment to stop; refresh the connection state again.
        clearTimeout(this.disconnectTimer)
        this.disconnectTimer = setTimeout(() => this.refreshClients(), 1000)
      },
      async unpairSingle(client) {
        try {
          const response = await apiFetch('./api/clients/unpair', jsonRequest({ uuid: client.uuid }))
          const body = await response.json()
          this.showUnpairMessage = body.status === true
        } catch (e) {
          console.error('Failed to unpair the client', e)
        }
        await this.refreshClients()
      },
      async unpairAll() {
        this.unpairAllPressed = true
        try {
          const response = await apiFetch('./api/clients/unpair-all', jsonRequest())
          this.unpairAllStatus = (await response.json()).status === true
        } catch (e) {
          this.unpairAllStatus = false
        } finally {
          this.unpairAllPressed = false
        }
        clearTimeout(this.unpairAllTimer)
        this.unpairAllTimer = setTimeout(() => {
          this.unpairAllStatus = null
        }, 5000)
        await this.refreshClients()
      },
    },
  }
</script>
