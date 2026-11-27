import { flushPromises, mount } from '@vue/test-utils'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

vi.mock('../../src_assets/common/assets/web/Navbar.vue', () => ({
  default: { template: '<div />' },
}))
vi.mock('../../src_assets/common/assets/web/fetch_utils', async () => ({
  ...(await vi.importActual('../../src_assets/common/assets/web/fetch_utils')),
  apiFetch: vi.fn(),
}))

import { apiFetch } from '../../src_assets/common/assets/web/fetch_utils'
import Pin from '../../src_assets/common/assets/web/Pin.vue'
import ClientManager from '../../src_assets/common/assets/web/pairing/ClientManager.vue'
import OtpPairing from '../../src_assets/common/assets/web/pairing/OtpPairing.vue'
import { buildClientUpdate, createClientDraft, isValidDisplayMode, normalizeClient, sortClients } from '../../src_assets/common/assets/web/pairing/clients'
import { buildDeepLink, guessHost, readSavedHost, saveHost } from '../../src_assets/common/assets/web/pairing/otp'
import { PERM, hasPermission, isSuppressed, permToStr, sanitizePermissions, togglePermission } from '../../src_assets/common/assets/web/pairing/permissions'

/**
 * @brief Build a JSON fetch response.
 *
 * @param {object} body Parsed body.
 * @param {number} [status] HTTP status.
 * @return {object} Response stand-in.
 */
function jsonResponse(body, status = 200) {
  return { status, ok: status === 200, json: async () => body }
}

const globalOptions = {
  mocks: { $t: (key, params) => (params ? `${key}:${JSON.stringify(params)}` : key) },
  provide: { i18n: { t: key => key } },
  stubs: { Checkbox: true },
}

beforeEach(() => {
  vi.clearAllMocks()
  sessionStorage.clear()
})

afterEach(() => {
  vi.useRealTimers()
  vi.unstubAllGlobals()
})

describe('client permissions', () => {
  // Bugs caught: 0, False positives: 0
  it('mirrors crypto::PERM', () => {
    expect(PERM._all).toBe(PERM._all_inputs | PERM._all_operations | PERM._all_actions)
    expect(PERM._default).toBe(PERM.view | PERM.list)
    expect(PERM._allow_view).toBe(PERM.view | PERM.launch)
  })

  // Bugs caught: 0, False positives: 0
  it('formats, toggles and restricts permission bits', () => {
    expect(permToStr(PERM._all)).toBe('07 1F 1F')
    expect(permToStr(PERM._default)).toBe('03 00 00')
    const perm = togglePermission(PERM._default, 'input_mouse')
    expect(hasPermission(perm, 'input_mouse')).toBe(true)
    expect(hasPermission(togglePermission(perm, 'input_mouse'), 'input_mouse')).toBe(false)
    expect(sanitizePermissions(0xFFFFFFFF)).toBe(PERM._all)
  })

  // Bugs caught: 0, False positives: 0
  it('treats List and View as implied by Launch', () => {
    expect(isSuppressed(PERM.launch, ['view', 'launch'])).toBe(true)
    expect(isSuppressed(PERM.list, ['view', 'launch'])).toBe(false)
  })
})

describe('client update payload', () => {
  const raw = {
    uuid: 'client-1',
    name: 'Deck',
    perm: 50331648,
    display_mode: '',
    enabled: true,
    connected: true,
    allow_client_commands: true,
    enable_legacy_ordering: false,
    always_use_virtual_display: false,
    do: [{ cmd: 'notify connect', elevated: false }],
  }

  // Bugs caught: 0, False positives: 0
  it('normalizes a client list entry with defaults', () => {
    expect(normalizeClient({ uuid: 'x', perm: '16777216' })).toEqual({
      uuid: 'x',
      name: '',
      perm: PERM.list,
      display_mode: '',
      enabled: true,
      connected: false,
      allow_client_commands: true,
      enable_legacy_ordering: true,
      always_use_virtual_display: false,
      do: [],
      undo: [],
    })
  })

  // Bugs caught: 0, False positives: 0
  it('sends Apollo per-client settings with trimmed values and only non-empty commands', () => {
    const draft = createClientDraft(normalizeClient(raw))
    draft.name = '  Steam Deck  '
    draft.display_mode = ' 1280x800x90 '
    draft.perm = 0xFFFFFFFF
    draft.always_use_virtual_display = true
    draft.undo.push({ cmd: '  notify disconnect ', elevated: 'true' }, { cmd: '   ', elevated: false })

    expect(buildClientUpdate(draft)).toEqual({
      uuid: 'client-1',
      name: 'Steam Deck',
      display_mode: '1280x800x90',
      allow_client_commands: true,
      enable_legacy_ordering: false,
      always_use_virtual_display: true,
      perm: PERM._all,
      do: [{ cmd: 'notify connect', elevated: false }],
      undo: [{ cmd: 'notify disconnect', elevated: true }],
    })
  })

  // Bugs caught: 0, False positives: 0
  it('edits a copy so cancelling leaves the client unchanged', () => {
    const client = normalizeClient(raw)
    const draft = createClientDraft(client)
    draft.do[0].cmd = 'changed'
    draft.name = 'changed'

    expect(client.do[0].cmd).toBe('notify connect')
    expect(client.name).toBe('Deck')
  })

  // Bugs caught: 0, False positives: 0
  it.each([
    ['', true],
    ['1920x1080x60', true],
    ['1920x1080x59.94', true],
    ['1920x1080', false],
    ['big', false],
  ])('validates display mode %s', (mode, valid) => {
    expect(isValidDisplayMode(mode)).toBe(valid)
  })

  // Bugs caught: 0, False positives: 0
  it('sorts clients by name with unnamed clients last', () => {
    expect(sortClients([{ name: '' }, { name: 'b' }, { name: 'A' }]).map(c => c.name)).toEqual(['A', 'b', ''])
  })
})

describe('client manager', () => {
  const listResponse = jsonResponse({
    status: true,
    platform: 'linux',
    named_certs: [
      { uuid: 'c1', name: 'Phone', perm: PERM._all, enabled: true, connected: true },
      { uuid: 'c2', name: 'Tablet', perm: PERM._default, enabled: false, connected: false },
    ],
  })

  /**
   * @brief Mount the client manager with the client list loaded.
   *
   * @return {Promise<object>} Wrapper.
   */
  async function mountManager() {
    apiFetch.mockResolvedValue(listResponse)
    const wrapper = mount(ClientManager, { global: globalOptions })
    await flushPromises()
    apiFetch.mockClear()
    apiFetch.mockResolvedValue(listResponse)
    return wrapper
  }

  // Bugs caught: 0, False positives: 0
  it('lists clients with permission badges, connection state and the enable switch', async () => {
    const wrapper = await mountManager()

    const rows = wrapper.findAll('.client-row')
    expect(rows).toHaveLength(2)
    expect(rows[0].text()).toContain('[ 07 1F 1F ]')
    expect(rows[0].text()).toContain('pin.client_connected')
    expect(rows[0].find('.btn-warning').exists()).toBe(true)
    expect(rows[1].find('.btn-warning').exists()).toBe(false)
    expect(wrapper.get('#toggle-c2').element.checked).toBe(false)
    wrapper.unmount()
  })

  // Bugs caught: 0, False positives: 0
  it('saves the edited client through /api/clients/update', async () => {
    const wrapper = await mountManager()
    apiFetch.mockResolvedValueOnce(jsonResponse({ status: true }))

    await wrapper.findAll('.client-row')[1].findAll('.btn-primary')[0].trigger('click')
    await wrapper.get('#perm-input_kbd-c2').trigger('click')
    await wrapper.get('#client-name-c2').setValue('Living room tablet')
    await wrapper.get('form.client-editor').trigger('submit')
    await flushPromises()

    expect(apiFetch.mock.calls[0][0]).toBe('./api/clients/update')
    expect(JSON.parse(apiFetch.mock.calls[0][1].body)).toEqual({
      uuid: 'c2',
      name: 'Living room tablet',
      display_mode: '',
      allow_client_commands: true,
      enable_legacy_ordering: true,
      always_use_virtual_display: false,
      perm: PERM._default | PERM.input_kbd,
      do: [],
      undo: [],
    })
    expect(apiFetch.mock.calls[1][0]).toBe('./api/clients/list')
    expect(wrapper.find('form.client-editor').exists()).toBe(false)
    wrapper.unmount()
  })

  // Bugs caught: 0, False positives: 0
  it('does not save an invalid display mode override', async () => {
    const wrapper = await mountManager()

    await wrapper.findAll('.client-row')[0].get('.btn-primary').trigger('click')
    await wrapper.get('#display_mode_override-c1').setValue('1920x1080')
    await wrapper.get('form.client-editor').trigger('submit')
    await flushPromises()

    expect(apiFetch).not.toHaveBeenCalled()
    expect(wrapper.get('#display_mode_override-c1').classes()).toContain('is-invalid')
    wrapper.unmount()
  })

  // Bugs caught: 0, False positives: 0
  it('keeps the editor and its changes open when the save is rejected', async () => {
    const wrapper = await mountManager()
    apiFetch.mockResolvedValueOnce(jsonResponse({ status: false, error: 'Disk full' }))

    await wrapper.findAll('.client-row')[1].get('.btn-primary').trigger('click')
    await wrapper.get('#client-name-c2').setValue('Living room tablet')
    await wrapper.get('form.client-editor').trigger('submit')
    await flushPromises()

    expect(wrapper.text()).toContain('Disk full')
    expect(wrapper.get('#client-name-c2').element.value).toBe('Living room tablet')
    wrapper.unmount()
  })

  // A successful save closes the submitted draft, so edits made while it is in flight would be lost.
  // Bugs caught: 0, False positives: 0
  it('locks the editor while its save is in flight', async () => {
    const wrapper = await mountManager()
    let answer
    apiFetch.mockImplementationOnce(() => new Promise(resolve => { answer = resolve }))

    await wrapper.findAll('.client-row')[1].get('.btn-primary').trigger('click')
    await wrapper.get('form.client-editor').trigger('submit')
    await flushPromises()

    for (const id of ['#client-name-c2', '#perm-input_kbd-c2', '#display_mode_override-c2']) {
      expect(wrapper.get(id).element.matches(':disabled'), id).toBe(true)
    }

    answer(jsonResponse({ status: false, error: 'Disk full' }))
    await flushPromises()
    expect(wrapper.get('#client-name-c2').element.matches(':disabled')).toBe(false)
    wrapper.unmount()
  })

  // Bugs caught: 0, False positives: 0
  it('keeps an open editor and the client list across a background refresh', async () => {
    const wrapper = await mountManager()

    await wrapper.findAll('.client-row')[0].get('.btn-primary').trigger('click')
    await wrapper.get('#client-name-c1').setValue('Renamed phone')
    await wrapper.vm.refreshClients()
    await flushPromises()
    expect(wrapper.get('#client-name-c1').element.value).toBe('Renamed phone')

    apiFetch.mockRejectedValueOnce(new Error('offline'))
    await wrapper.vm.refreshClients()
    await flushPromises()
    expect(wrapper.get('#client-name-c1').element.value).toBe('Renamed phone')
    expect(wrapper.findAll('.client-row')).toHaveLength(1)
    wrapper.unmount()
  })

  // Bugs caught: 0, False positives: 0
  it('enables, disconnects and unpairs by UUID', async () => {
    const wrapper = await mountManager()

    await wrapper.get('#toggle-c2').trigger('change')
    await flushPromises()
    await wrapper.findAll('.client-row')[0].get('.btn-warning').trigger('click')
    await flushPromises()
    apiFetch.mockResolvedValueOnce(jsonResponse({ status: true }))
    await wrapper.findAll('.client-row')[1].get('.btn-danger').trigger('click')
    await flushPromises()

    const posts = apiFetch.mock.calls.filter(([, options]) => options?.method === 'POST')
    expect(posts.map(([url, options]) => [url, JSON.parse(options.body)])).toEqual([
      ['./api/clients/update', { uuid: 'c2', enabled: true }],
      ['./api/clients/disconnect', { uuid: 'c1' }],
      ['./api/clients/unpair', { uuid: 'c2' }],
    ])
    expect(wrapper.vm.showUnpairMessage).toBe(true)
    wrapper.unmount()
  })
})

describe('PIN pairing', () => {
  // Bugs caught: 0, False positives: 0
  it('sends the selected pairing_id with the PIN and the trimmed device name', async () => {
    vi.useFakeTimers()
    const refreshClients = vi.fn()
    const ctx = {
      selectedPairingId: '0123456789abcdef0123456789abcdef',
      pin: '1234',
      name: '  My Phone ',
      status: null,
      i18n: { t: key => key },
      loadPendingPairings: vi.fn(),
      $refs: { clientManager: { refreshClients } },
    }
    apiFetch.mockResolvedValueOnce(jsonResponse({ status: true }))

    await Pin.methods.registerDevice.call(ctx)

    expect(apiFetch).toHaveBeenCalledExactlyOnceWith('./api/pin', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ pairing_id: '0123456789abcdef0123456789abcdef', pin: '1234', name: 'My Phone' }),
    })
    expect(ctx.status).toEqual({ type: 'success', message: 'pin.pair_success_check_perm' })
    expect(ctx.pin).toBe('')
    expect(ctx.name).toBe('')
    vi.advanceTimersByTime(1000)
    expect(refreshClients).toHaveBeenCalledOnce()
  })

  // Bugs caught: 0, False positives: 0
  it('requires a selected pairing request', async () => {
    const ctx = { selectedPairingId: '', status: null, i18n: { t: key => key } }

    await Pin.methods.registerDevice.call(ctx)

    expect(apiFetch).not.toHaveBeenCalled()
    expect(ctx.status.type).toBe('danger')
  })

  // Bugs caught: 0, False positives: 0
  it('opens on OTP pairing unless the address asks for PIN pairing', () => {
    vi.stubGlobal('location', { hash: '' })
    expect(Pin.data().currentTab).toBe('OTP')
    vi.stubGlobal('location', { hash: '#PIN' })
    expect(Pin.data().currentTab).toBe('PIN')
  })
})

describe('OTP pairing', () => {
  // Bugs caught: 0, False positives: 0
  it('builds the Artemis deep link', () => {
    expect(buildDeepLink({ hostAddr: '192.168.1.5', hostPort: 47989, otp: '1234', passphrase: 'abcd', hostName: 'My PC' }))
      .toBe('art://192.168.1.5:47989?pin=1234&passphrase=abcd&name=My%20PC')
  })

  // Bugs caught: 0, False positives: 0
  it('brackets IPv6 hosts and encodes each query value of the deep link', () => {
    const link = { hostPort: 47989, otp: '1234', passphrase: 'abcd', hostName: 'Gaming PC #2 & co' }

    expect(buildDeepLink({ ...link, hostAddr: 'fe80::1' }))
      .toBe('art://[fe80::1]:47989?pin=1234&passphrase=abcd&name=Gaming%20PC%20%232%20%26%20co')
    expect(buildDeepLink({ ...link, hostAddr: '[::1]' })).toMatch(/^art:\/\/\[::1\]:47989\?/)
  })

  // Bugs caught: 0, False positives: 0
  it('guesses the host from the page address, or the server address on the host itself', () => {
    expect(guessHost('192.168.1.5', { hostname: 'apollo.lan', port: '47990' })).toEqual({ hostAddr: 'apollo.lan', hostPort: 47989 })
    expect(guessHost('192.168.1.5', { hostname: 'localhost', port: '47990' })).toEqual({ hostAddr: '192.168.1.5', hostPort: 47989 })
    expect(guessHost('192.168.1.5', { hostname: 'localhost', port: '' })).toEqual({ hostAddr: '192.168.1.5', hostPort: 47989 })
  })

  // Bugs caught: 0, False positives: 0
  it('remembers a host entered by hand for the session', () => {
    expect(readSavedHost(sessionStorage)).toBeNull()
    saveHost(sessionStorage, { hostAddr: '10.0.0.2', hostPort: '50000' })
    expect(readSavedHost(sessionStorage)).toEqual({ hostAddr: '10.0.0.2', hostPort: '50000' })
  })

  // Bugs caught: 0, False positives: 0
  it('requests a PIN with the passphrase and device name and shows it', async () => {
    vi.stubGlobal('location', { hostname: 'localhost', port: '47990', hash: '' })
    vi.stubGlobal('QRCode', vi.fn())
    apiFetch.mockResolvedValueOnce(jsonResponse({ status: true, otp: '4321', ip: '192.168.1.5', name: 'My PC' }))
    const wrapper = mount(OtpPairing, { global: globalOptions })

    await wrapper.get('#otp-passphrase').setValue('abcd')
    await wrapper.get('#otp-device-name').setValue('Phone')
    await wrapper.get('form').trigger('submit')
    await flushPromises()

    expect(apiFetch).toHaveBeenCalledExactlyOnceWith('./api/otp', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ passphrase: 'abcd', deviceName: 'Phone' }),
    })
    expect(wrapper.get('.otp-display').text()).toBe('4321')
    expect(wrapper.text()).toContain('art://192.168.1.5:47989')
    expect(QRCode).toHaveBeenCalledOnce()
    wrapper.unmount()
  })

  // The server binds the PIN to the passphrase it was requested with.
  // Bugs caught: 0, False positives: 0
  it('keeps the requested passphrase in the link when the field is edited afterwards', async () => {
    vi.stubGlobal('location', { hostname: 'localhost', port: '47990', hash: '' })
    vi.stubGlobal('QRCode', vi.fn())
    apiFetch.mockResolvedValueOnce(jsonResponse({ status: true, otp: '4321', ip: '192.168.1.5', name: 'My PC' }))
    const wrapper = mount(OtpPairing, { global: globalOptions })

    await wrapper.get('#otp-passphrase').setValue('abcd')
    await wrapper.get('form').trigger('submit')
    await flushPromises()
    await wrapper.get('#otp-passphrase').setValue('wxyz')

    expect(wrapper.vm.deepLink).toContain('passphrase=abcd')
    wrapper.unmount()
  })

  // Bugs caught: 0, False positives: 0
  it('opens the host editor when the server cannot tell its address', async () => {
    vi.useFakeTimers()
    vi.stubGlobal('location', { hostname: 'localhost', port: '47990', hash: '' })
    vi.stubGlobal('QRCode', vi.fn())
    apiFetch.mockResolvedValueOnce(jsonResponse({ status: true, otp: '4321', ip: '', name: 'My PC' }))
    const wrapper = mount(OtpPairing, { global: globalOptions })

    await wrapper.get('#otp-passphrase').setValue('abcd')
    await wrapper.get('form').trigger('submit')
    await flushPromises()
    expect(wrapper.find('#otp-host').exists()).toBe(true)

    vi.runOnlyPendingTimers()
    await flushPromises()
    expect(wrapper.find('#otp-host').exists()).toBe(false)
    wrapper.unmount()
  })

  // Bugs caught: 0, False positives: 0
  it('shows the server error when the passphrase is rejected', async () => {
    apiFetch.mockResolvedValueOnce(jsonResponse({ status: false, error: 'Passphrase too short!' }, 400))
    const wrapper = mount(OtpPairing, { global: globalOptions })

    await wrapper.get('#otp-passphrase').setValue('ab')
    await wrapper.get('form').trigger('submit')
    await flushPromises()

    expect(wrapper.get('.alert-danger').text()).toBe('Passphrase too short!')
    wrapper.unmount()
  })
})

describe('pairing page', () => {
  // Bugs caught: 0, False positives: 0
  it('shows OTP and PIN pairing with the client list', async () => {
    vi.stubGlobal('location', { hash: '', hostname: 'localhost', port: '47990' })
    apiFetch.mockImplementation(async url => jsonResponse(url === './api/pin'
      ? { pairings: [{ id: '0123456789abcdef0123456789abcdef', name: 'Moonlight PC', address: '192.168.1.9' }] }
      : { status: true, platform: 'linux', named_certs: [{ uuid: 'c1', name: 'Phone', perm: PERM._default }] }))
    const wrapper = mount(Pin, { global: globalOptions })
    await flushPromises()

    expect(wrapper.find('#otp-passphrase').exists()).toBe(true)
    expect(wrapper.findAll('.client-row')).toHaveLength(1)

    await wrapper.findAll('.nav-link')[1].trigger('click')
    expect(wrapper.find('#otp-passphrase').exists()).toBe(false)
    expect(wrapper.get('#pairing-input').element.value).toBe('0123456789abcdef0123456789abcdef')
    expect(wrapper.get('#name-input').attributes('placeholder')).toBe('pin.device_name (Moonlight PC)')
    expect(wrapper.get('#name-input').attributes('required')).toBeUndefined()
    wrapper.unmount()
  })
})
