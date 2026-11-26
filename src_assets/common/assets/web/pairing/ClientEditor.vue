<template>
  <form class="client-editor" @submit.prevent="save">
    <div class="d-flex align-items-center gap-2 mb-3">
      <span class="badge" :class="permBadgeClass">[ {{ permToStr(draft.perm) }} ]</span>
      <label :for="`client-name-${draft.uuid}`" class="visually-hidden">{{ $t('pin.device_name') }}</label>
      <input :id="`client-name-${draft.uuid}`" v-model="draft.name" class="form-control flex-grow-1" type="text"
             :placeholder="$t('pin.device_name')">
      <button type="submit" class="btn btn-success" :disabled="busy" :title="$t('_common.save')">
        <check :size="18" class="icon"></check>
      </button>
      <button type="button" class="btn btn-secondary" :disabled="busy" :title="$t('_common.cancel')" @click="$emit('cancel')">
        <x :size="18" class="icon"></x>
      </button>
    </div>

    <!-- Permissions -->
    <div class="d-flex flex-wrap justify-content-center gap-4 mb-3">
      <div v-for="group in permissionGroups" :key="group.name" class="d-flex flex-column">
        <div class="fw-semibold mb-1">{{ $t(`pin.permission_group_${group.name.toLowerCase()}`) }}</div>
        <button v-for="perm in group.permissions" :key="perm.name" type="button" class="my-1 btn btn-sm"
                :id="`perm-${perm.name}-${draft.uuid}`"
                :disabled="isSuppressed(draft.perm, perm.suppressedBy)"
                :aria-pressed="permissionShown(perm)"
                :class="permissionShown(perm) ? 'btn-success' : 'btn-outline-secondary'"
                @click="draft.perm = togglePermission(draft.perm, perm.name)">
          {{ $t(`permissions.${perm.name}`) }}
        </button>
      </div>
    </div>

    <!-- Legacy ordering -->
    <Checkbox class="mb-3"
              :id="`enable_legacy_ordering-${draft.uuid}`"
              label="pin.enable_legacy_ordering"
              desc="pin.enable_legacy_ordering_desc"
              v-model="draft.enable_legacy_ordering"
              default="true"
    ></Checkbox>

    <!-- Always use a virtual display -->
    <Checkbox v-if="platform === 'windows' || platform === 'linux'"
              class="mb-3"
              :id="`always_use_virtual_display-${draft.uuid}`"
              label="pin.always_use_virtual_display"
              desc="pin.always_use_virtual_display_desc"
              v-model="draft.always_use_virtual_display"
              default="false"
    ></Checkbox>

    <!-- Display mode override -->
    <div class="mb-3">
      <label :for="`display_mode_override-${draft.uuid}`" class="form-label">{{ $t('pin.display_mode_override') }}</label>
      <input :id="`display_mode_override-${draft.uuid}`" type="text" class="form-control"
             :class="{ 'is-invalid': !displayModeValid }"
             v-model="draft.display_mode" placeholder="1920x1080x59.94" />
      <div class="invalid-feedback" v-if="!displayModeValid">{{ $t('pin.display_mode_override_error') }}</div>
      <div class="form-text">
        {{ $t('pin.display_mode_override_desc') }}
        <a :href="wikiLinks.displayMode" target="_blank" rel="noopener noreferrer">{{ $t('_common.learn_more') }}</a>
      </div>
    </div>

    <!-- Client commands -->
    <Checkbox class="mb-3"
              :id="`allow_client_commands-${draft.uuid}`"
              label="pin.allow_client_commands"
              desc="pin.allow_client_commands_desc"
              v-model="draft.allow_client_commands"
              default="true"
    ></Checkbox>

    <template v-if="draft.allow_client_commands">
      <div v-for="cmdType in ['do', 'undo']" :key="cmdType" class="mb-3 d-flex flex-column">
        <div class="form-label mb-0">{{ $t(`pin.client_${cmdType}_cmd`) }}</div>
        <div class="form-text">
          {{ $t(`pin.client_${cmdType}_cmd_desc`) }}
          <a :href="wikiLinks.clientCommands" target="_blank" rel="noopener noreferrer">{{ $t('_common.learn_more') }}</a>
        </div>
        <table class="mt-2 table" v-if="draft[cmdType].length > 0">
          <thead>
            <tr>
              <th scope="col" class="w-75"><terminal :size="16" class="icon"></terminal> {{ $t('_common.cmd_val') }}</th>
              <th scope="col" v-if="canElevate"><shield :size="16" class="icon"></shield> {{ $t('_common.run_as') }}</th>
              <th scope="col"></th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="(c, i) in draft[cmdType]" :key="i">
              <td>
                <label :for="`client-${cmdType}-cmd-${draft.uuid}-${i}`" class="visually-hidden">{{ $t('_common.cmd_val') }}</label>
                <input :id="`client-${cmdType}-cmd-${draft.uuid}-${i}`" type="text" class="form-control monospace" v-model="c.cmd" />
              </td>
              <td v-if="canElevate" class="align-middle">
                <div class="form-check">
                  <input type="checkbox" class="form-check-input" :id="`client-${cmdType}-cmd-admin-${draft.uuid}-${i}`" v-model="c.elevated" />
                  <label :for="`client-${cmdType}-cmd-admin-${draft.uuid}-${i}`" class="form-check-label">{{ $t('_common.elevated') }}</label>
                </div>
              </td>
              <td class="text-end text-nowrap">
                <button type="button" class="btn btn-danger me-2" @click="draft[cmdType].splice(i, 1)">
                  <trash-2 :size="16" class="icon"></trash-2>
                </button>
                <button type="button" class="btn btn-success" @click="addCommand(draft[cmdType], i + 1)">
                  <plus :size="16" class="icon"></plus>
                </button>
              </td>
            </tr>
          </tbody>
        </table>
        <div>
          <button type="button" class="btn btn-success mt-2" @click="addCommand(draft[cmdType], draft[cmdType].length)">
            <plus :size="16" class="icon"></plus> {{ $t('config.add') }}
          </button>
        </div>
      </div>
    </template>
  </form>
</template>

<script>
  import { Check, Plus, Shield, Terminal, Trash2, X } from '@lucide/vue'
  import Checkbox from '../Checkbox.vue'
  import { apolloWikiPage } from '../links'
  import { isValidDisplayMode } from './clients'
  import { PERMISSION_GROUPS, hasPermission, isPowerful, isSuppressed, permToStr, togglePermission } from './permissions'

  export default {
    components: { Check, Checkbox, Plus, Shield, Terminal, Trash2, X },
    props: {
      /** Editable client copy (see createClientDraft); edited in place. */
      draft: { type: Object, required: true },
      platform: { type: String, default: '' },
      busy: { type: Boolean, default: false },
    },
    emits: ['save', 'cancel'],
    data() {
      return {
        permissionGroups: PERMISSION_GROUPS,
        wikiLinks: {
          clientCommands: apolloWikiPage('Client-Commands'),
          displayMode: apolloWikiPage('Display-Mode-Override'),
        },
      }
    },
    computed: {
      canElevate() {
        return this.platform === 'windows' || this.platform === 'linux'
      },
      displayModeValid() {
        return isValidDisplayMode(this.draft.display_mode)
      },
      permBadgeClass() {
        return isPowerful(this.draft.perm) ? 'bg-danger' : 'bg-primary'
      },
    },
    methods: {
      isSuppressed,
      permToStr,
      togglePermission,
      /**
       * @brief Whether a permission shows as granted, directly or through a broader one.
       *
       * @param {{name: string, suppressedBy: string[]}} perm Permission entry.
       * @return {boolean} Whether to highlight it.
       */
      permissionShown(perm) {
        return isSuppressed(this.draft.perm, perm.suppressedBy) || hasPermission(this.draft.perm, perm.name)
      },
      /**
       * @brief Insert an empty command.
       *
       * @param {Array} commands Command list.
       * @param {number} index Insert position.
       */
      addCommand(commands, index) {
        commands.splice(index, 0, { cmd: '', elevated: false })
      },
      save() {
        if (!this.displayModeValid) {
          return
        }
        this.$emit('save', this.draft)
      },
    },
  }
</script>
