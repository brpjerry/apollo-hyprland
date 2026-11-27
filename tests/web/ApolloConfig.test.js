import { mount } from '@vue/test-utils'
import { readFileSync } from 'node:fs'
import { dirname, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { describe, expect, it, vi } from 'vitest'
import { ref } from 'vue'

vi.mock('../../src_assets/common/assets/web/Navbar.vue', () => ({
  default: { template: '<div />' },
}))

import Config, { cleanConfigForSave, parseJsonOptions } from '../../src_assets/common/assets/web/Config.vue'
import configTabs from '../../src_assets/common/assets/web/configs/config_tabs.json'
import Advanced from '../../src_assets/common/assets/web/configs/tabs/Advanced.vue'
import AudioVideo from '../../src_assets/common/assets/web/configs/tabs/AudioVideo.vue'
import General from '../../src_assets/common/assets/web/configs/tabs/General.vue'
import Inputs from '../../src_assets/common/assets/web/configs/tabs/Inputs.vue'

const projectRoot = resolve(dirname(fileURLToPath(import.meta.url)), '../..')

/**
 * @brief Mount a configuration tab with the defaults from config_tabs.json.
 *
 * @param {object} component Tab component.
 * @param {string} tabId Tab id in config_tabs.json.
 * @param {string} platform Host platform.
 * @param {object} [overrides] Config values to change.
 * @param {object} [props] Extra props.
 * @return {{wrapper: object, config: object}} Mounted tab and its config.
 */
function mountTab(component, tabId, platform, overrides = {}, props = {}) {
  const config = { ...structuredClone(configTabs.find(tab => tab.id === tabId).options), ...overrides }
  const wrapper = mount(component, {
    props: { platform, config, ...props },
    global: {
      mocks: { $t: (key, params) => (params ? `${key}:${JSON.stringify(params)}` : key) },
      provide: { platform: ref(platform), i18n: { t: key => key } },
      stubs: {
        AdapterNameSelector: true,
        DisplayOutputSelector: true,
        DisplayDeviceOptions: true,
        DisplayModesSettings: true,
      },
    },
  })
  return { wrapper, config }
}

describe('Apollo configuration options', () => {
  // Bugs caught: 0, False positives: 0
  it('gives every option the server parses a default in config_tabs.json', () => {
    const configCpp = readFileSync(resolve(projectRoot, 'src/config.cpp'), 'utf8')
    const parsed = new Set([...configCpp.matchAll(/\(vars, *"([a-z0-9_]+)"/g)].map(match => match[1]))
    const tabKeys = new Set(configTabs.flatMap(tab => Object.keys(tab.options)))
    // Options set through other pages, or only in the config file.
    const elsewhere = new Set()

    expect([...parsed].filter(key => !tabKeys.has(key) && !elsewhere.has(key))).toEqual([])
  })

  // Bugs caught: 0, False positives: 0
  it('uses the server defaults for the Apollo options', () => {
    const options = Object.assign({}, ...configTabs.map(tab => tab.options))
    expect(options).toMatchObject({
      global_state_cmd: [],
      server_cmd: [],
      elevation_method: 'pkexec',
      enable_pairing: 'enabled',
      enable_discovery: 'enabled',
      hide_tray_controls: 'disabled',
      enable_input_only_mode: 'disabled',
      forward_rumble: 'enabled',
      keep_sink_default: 'enabled',
      auto_capture_sink: 'enabled',
      headless_mode: 'disabled',
      double_refreshrate: 'disabled',
      isolated_virtual_display_option: 'disabled',
      limit_framerate: 'enabled',
      envvar_compatibility_mode: 'disabled',
      legacy_ordering: 'disabled',
      ignore_encoder_probe_failure: 'disabled',
      nvenc_intra_refresh: 'disabled',
    })
  })

  // Bugs caught: 0, False positives: 0
  it('parses the JSON command lists from GET /api/config', () => {
    const config = {
      global_prep_cmd: '[{"do":"a","undo":"b","elevated":"true"}]',
      global_state_cmd: '[{"do":"resume","undo":"pause"}]',
      server_cmd: '[{"name":"Lock","cmd":"loginctl lock-session","elevated":false}]',
      dd_mode_remapping: 'not json',
    }

    parseJsonOptions(config)

    expect(config.global_prep_cmd).toEqual([{ do: 'a', undo: 'b', elevated: true }])
    expect(config.global_state_cmd).toEqual([{ do: 'resume', undo: 'pause', elevated: false }])
    expect(config.server_cmd).toEqual([{ name: 'Lock', cmd: 'loginctl lock-session', elevated: false }])
    expect(config).not.toHaveProperty('dd_mode_remapping')
  })

  // The server reads elevated with Boost's bool parser (1 and "1" are true) and runs server commands by position,
  // so saving an unrelated setting must not change either.
  // Bugs caught: 0, False positives: 0
  it('keeps the commands and elevation flags the server accepts', () => {
    const config = {
      global_prep_cmd: JSON.stringify([{ do: 'a', undo: '', elevated: 1 }, { do: 'b', undo: '', elevated: '1' }, { do: 'c', undo: '', elevated: 0 }]),
      server_cmd: JSON.stringify([{ name: '', cmd: 'loginctl lock-session' }, { name: 'Suspend', cmd: 'systemctl suspend' }]),
    }

    parseJsonOptions(config)
    cleanConfigForSave(config)

    expect(config.global_prep_cmd.map(cmd => cmd.elevated)).toEqual([true, true, false])
    expect(config.server_cmd.map(cmd => cmd.cmd)).toEqual(['loginctl lock-session', 'systemctl suspend'])
  })

  // Bugs caught: 0, False positives: 0
  it('drops empty command rows and an invalid fallback mode before saving', () => {
    const config = cleanConfigForSave({
      global_prep_cmd: [{ do: ' ', undo: '' }, { do: 'x', undo: '' }],
      global_state_cmd: [{ do: '', undo: 'y' }, { do: '', undo: '  ' }],
      server_cmd: [{ name: 'Lock', cmd: 'lock' }, { name: '', cmd: ' ' }],
      fallback_mode: '1920x1080',
    })

    expect(config.global_prep_cmd).toEqual([{ do: 'x', undo: '' }])
    expect(config.global_state_cmd).toEqual([{ do: '', undo: 'y' }])
    expect(config.server_cmd).toEqual([{ name: 'Lock', cmd: 'lock' }])
    expect(config).not.toHaveProperty('fallback_mode')
    expect(cleanConfigForSave({ fallback_mode: '2560x1440x120' }).fallback_mode).toBe('2560x1440x120')
  })

  // Clients run server commands by position, and the server keeps rows without a command: dropping one in the middle
  // would make the next button run another command.
  // Bugs caught: 0, False positives: 0
  it('keeps the position of every server command when saving', () => {
    const rows = [
      { name: 'Unfinished', cmd: '' },
      { name: '', cmd: '' },
      { name: 'Lock', cmd: 'lock' },
      { name: '', cmd: '' },
      { name: ' ', cmd: '' },
    ]
    const config = cleanConfigForSave({ server_cmd: structuredClone(rows) })

    expect(config.server_cmd).toEqual(rows.slice(0, 3))
    expect(cleanConfigForSave({ server_cmd: [{ name: '', cmd: '' }] }).server_cmd).toEqual([])
  })

  // The server reads a hand-edited numeric label as text, so saving must not trip over it.
  // Bugs caught: 0, False positives: 0
  it('saves server commands whose fields are numbers', () => {
    const rows = [{ name: 'Lock', cmd: 'lock' }, { name: 1, cmd: 2 }, { name: 1, cmd: '' }, { name: '', cmd: null }]
    const config = cleanConfigForSave({ server_cmd: structuredClone(rows) })

    expect(config.server_cmd).toEqual(rows.slice(0, 3))
  })

  // Saving without the invalid value would remove the previously saved mode from the configuration file.
  // Bugs caught: 0, False positives: 0
  it('stops Save and Apply at an invalid fallback mode and shows its tab', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => ({ status: 200 })))
    const ctx = {
      config: { fallback_mode: '1920x1080' },
      currentTab: 'general',
      tabs: [],
      saved: false,
      restarted: false,
      serialize: Config.methods.serialize,
    }
    ctx.save = Config.methods.save.bind(ctx)

    expect(await Config.methods.save.call(ctx)).toBe(false)
    Config.methods.apply.call(ctx)
    await Promise.resolve()

    expect(fetch).not.toHaveBeenCalled()
    expect(ctx.currentTab).toBe('av')
    expect(ctx.restarted).toBe(false)
    vi.unstubAllGlobals()
  })

  // Bugs caught: 0, False positives: 0
  it('edits state and server commands and shows the Linux-only general options', async () => {
    const { wrapper, config } = mountTab(General, 'general', 'linux')

    expect(wrapper.find('#global_prep_cmd').exists()).toBe(true)
    expect(wrapper.find('#global_state_cmd').exists()).toBe(true)
    expect(wrapper.find('#server_cmd').exists()).toBe(true)
    expect(wrapper.find('#elevation_method').exists()).toBe(true)
    for (const id of ['enable_pairing', 'enable_discovery', 'hide_tray_controls']) {
      expect(wrapper.find(`#${id}`).exists()).toBe(true)
    }

    await wrapper.get('#global_state_cmd button.btn-success').trigger('click')
    await wrapper.get('#server_cmd button.btn-success').trigger('click')
    expect(config.global_state_cmd).toEqual([{ do: '', undo: '', elevated: false }])
    expect(config.server_cmd).toEqual([{ name: '', cmd: '', elevated: false }])

    await wrapper.get('#server-cmd-name-0').setValue('Lock')
    expect(config.server_cmd[0].name).toBe('Lock')
    wrapper.unmount()

    const macos = mountTab(General, 'general', 'macos').wrapper
    expect(macos.find('#elevation_method').exists()).toBe(false)
    macos.unmount()
  })

  // Bugs caught: 0, False positives: 0
  it.each([
    [0, 'alert-success', 'config.vdisplay_status_ready'],
    [-1, 'alert-warning', 'config.vdisplay_status_failed_linux'],
    [undefined, 'alert-warning', 'config.vdisplay_status_unavailable_linux'],
  ])('shows the Hyprland virtual display status %s', (vdisplay, alertClass, statusKey) => {
    const { wrapper } = mountTab(AudioVideo, 'av', 'linux', {}, { vdisplay })

    const status = wrapper.get('#vdisplay_status')
    expect(status.classes()).toContain(alertClass)
    expect(status.text()).toContain(`config.vdisplay_status_linux:${JSON.stringify({ status: statusKey })}`)
    for (const id of ['virtual_sink', 'keep_sink_default', 'auto_capture_sink', 'fallback_mode', 'headless_mode', 'double_refreshrate']) {
      expect(wrapper.find(`#${id}`).exists()).toBe(true)
    }
    wrapper.unmount()
  })

  // Bugs caught: 0, False positives: 0
  it('flags an invalid fallback display mode', async () => {
    const { wrapper } = mountTab(AudioVideo, 'av', 'linux')

    await wrapper.get('#fallback_mode').setValue('1920x1080')
    expect(wrapper.get('#fallback_mode').classes()).toContain('is-invalid')
    await wrapper.get('#fallback_mode').setValue('1920x1080x60')
    expect(wrapper.get('#fallback_mode').classes()).not.toContain('is-invalid')
    wrapper.unmount()
  })

  // Bugs caught: 0, False positives: 0
  it('shows the Apollo input and advanced options', () => {
    const inputs = mountTab(Inputs, 'input', 'linux').wrapper
    for (const id of ['always_send_scancodes', 'enable_input_only_mode', 'forward_rumble']) {
      expect(inputs.find(`#${id}`).exists()).toBe(true)
    }
    inputs.unmount()

    const advanced = mountTab(Advanced, 'advanced', 'linux').wrapper
    for (const id of ['limit_framerate', 'envvar_compatibility_mode', 'legacy_ordering', 'ignore_encoder_probe_failure']) {
      expect(advanced.find(`#${id}`).exists()).toBe(true)
    }
    expect(advanced.get('#capture').findAll('option').map(option => option.attributes('value'))).toContain('ext')
    advanced.unmount()
  })
})
