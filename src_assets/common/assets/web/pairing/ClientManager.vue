<template>
  <div class="card my-4 align-self-stretch" id="clients">
    <div class="card-body">
      <div class="d-flex justify-content-between align-items-center mb-3 gap-2">
        <h2 id="unpair" class="mb-0">{{ $t('pin.device_management') }}</h2>
        <button type="button" class="btn btn-danger" :disabled="unpairAllPressed || (clients.length === 0 && !unpairPending)" @click="unpairAll">
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
      <!-- The server's reason when a change could not be saved, e.g. a revocation that is in effect but not persisted. -->
      <div class="alert alert-danger mt-3" v-if="actionError">
        {{ actionError }}
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
        actionError: '',
        saving: false,
        showUnpairMessage: false,
        unpairAllPressed: false,
        unpairAllStatus: null,
        // A client was unpaired but the state file couldn't be saved, so it would be paired again after a restart:
        // Unpair All stays available, even for an empty list, to retry the save (it saves the whole client state).
        // The client list says so (`unpair_pending`), so this survives a reload of the page.
        unpairPending: false,
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
            // The server knows about an unsaved unpair even after this page was reloaded.
            this.unpairPending = body.unpair_pending ?? this.unpairPending
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
          const update = buildClientUpdate(draft)
          const response = await apiFetch('./api/clients/update', jsonRequest(update))
          const body = await response.json()
          if (body.status || body.applied) {
            // The server has these settings now (the editor sends no enable change, so `applied` is about them).
            // Applied to the row here too: when the refresh below fails, the next edit must not start from, and
            // send back, permissions or commands the server already took away.
            const client = this.clients.find(client => client.uuid === update.uuid)
            if (client) {
              Object.assign(client, update)
            }
          }
          if (!body.status) {
            if (body.applied) {
              // A revocation took effect although it couldn't be saved: reconcile the list with the server too
              // (connection state, other changes). The open draft is kept.
              await this.refreshClients()
            }
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
       * @brief Show the server's error for a client change that could not be saved.
       *
       * A revocation that could not be saved stays in effect until restart (`applied: true`); a grant is not made.
       *
       * @param {object} body Response body of a client change.
       */
      noteActionError(body) {
        if (body && body.status !== true && body.error) {
          this.actionError = body.error
        }
        // Covers an unpair whose follow-up list request fails; a successful one reports `unpair_pending` itself.
        if (body && body.status !== true && body.applied === true) {
          this.unpairPending = true
        }
      },
      /**
       * @brief Enable or disable a client (Sunshine's switch). Disabling ends its stream.
       *
       * @param {object} client Client to change.
       * @param {boolean} enabled New state.
       * @return {Promise<void>} Completion of the update and refresh.
       */
      async toggleClient(client, enabled) {
        this.actionError = ''
        try {
          const response = await apiFetch('./api/clients/update', jsonRequest({ uuid: client.uuid, enabled }))
          this.noteActionError(await response.json())
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
        this.actionError = ''
        try {
          const response = await apiFetch('./api/clients/unpair', jsonRequest({ uuid: client.uuid }))
          const body = await response.json()
          this.showUnpairMessage = body.status === true
          this.noteActionError(body)
        } catch (e) {
          console.error('Failed to unpair the client', e)
        }
        await this.refreshClients()
      },
      async unpairAll() {
        this.unpairAllPressed = true
        try {
          this.actionError = ''
          const response = await apiFetch('./api/clients/unpair-all', jsonRequest())
          const body = await response.json()
          this.unpairAllStatus = body.status === true
          if (body.status === true) {
            this.unpairPending = false
          }
          this.noteActionError(body)
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
